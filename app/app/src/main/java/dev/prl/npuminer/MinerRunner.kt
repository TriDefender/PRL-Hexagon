package dev.prl.npuminer

import android.content.Context
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

enum class MinePhase { IDLE, STARTING, RUNNING, STOPPED, ERROR }

data class MinerUiState(
    val phase: MinePhase = MinePhase.IDLE,
    val connected: Boolean = false,
    val gmacs: Double = 0.0,
    val history: List<Double> = emptyList(),
    val attempts: Long = 0,
    val accepted: Long = 0,
    val rejected: Long = 0,
    val startedAtMs: Long = 0,
    val lastStopReason: String = "",
    val log: List<String> = emptyList(),
)

/**
 * Run the bundled v28 compound miner as a plain app-uid child process.
 */
class MinerRunner(ctx: Context) {

    private val appCtx = ctx.applicationContext
    private val _state = MutableStateFlow(MinerUiState())
    val state: StateFlow<MinerUiState> = _state

    private var proc: Process? = null
    private val running = AtomicBoolean(false)
    private var readerThread: Thread? = null

    /** Notified with the miner child PID (null when it exits) for CPU sampling. */
    @Volatile var childPidListener: ((Int?) -> Unit)? = null

    val workDir: File get() = File(appCtx.filesDir, "work")
    val nativeDir: String get() = appCtx.applicationInfo.nativeLibraryDir
    val minerBin: File get() = File(nativeDir, MINER)
    val goldenFile: File get() = File(workDir, GOLDEN)
    private val pidFile: File get() = File(workDir, "miner.pid")

    val skelNames = listOf(FUSED_SKEL, COMPOUND_SKEL)

    val payloadReady: Boolean
        get() = minerBin.exists() && minerBin.canExecute() &&
                skelNames.all { File(nativeDir, it).exists() }

    /** Copy the golden vectors into app-private storage and verify the payload. */
    fun prepare(): Boolean {
        return try {
            workDir.mkdirs()
            appCtx.assets.open(GOLDEN).use { ins ->
                goldenFile.outputStream().use { os -> ins.copyTo(os) }
            }
            payloadReady
        } catch (_: Exception) {
            false
        }
    }

    private fun writeScript(name: String, body: String): File {
        val f = File(workDir, name)
        f.writeText("#!/system/bin/sh\n$body")
        f.setExecutable(true)
        return f
    }

    private fun envExports(): String = buildString {
        // libcdsprpc treats ADSP_LIBRARY_PATH as a single directory (it does
        // not split on ':').
        append("export ADSP_LIBRARY_PATH='$nativeDir'\n")
        // /system/lib64 must precede /vendor/lib64: vendor libbinder.so lacks
        // symbols that system libbinder_ndk expects, and the exec'd child gets
        // the plain linker namespace.
        append("export LD_LIBRARY_PATH='$nativeDir:/system/lib64:/vendor/lib64'\n")
    }

    /**
     * This is the real NPU/FastRPC capability check: rc==0 and "selftest PASSED".
     */
    fun selftest(timeoutSec: Long = 90): Pair<Boolean, String> {
        if (!prepare()) return false to "APK 载荷不完整（miner / skeleton / golden 缺失）"
        val script = writeScript(
            "selftest.sh",
            buildString {
                append("cd '$nativeDir' || exit 41\n")
                append(envExports())
                append("exec '${minerBin.absolutePath}' --selftest '${goldenFile.absolutePath}'\n")
            },
        )
        val p = try {
            ProcessBuilder("sh", script.absolutePath).redirectErrorStream(true).start()
        } catch (e: Exception) {
            return false to "无法启动自检进程: ${e.message}"
        }
        val out = StringBuilder()
        val reader = Thread {
            try {
                p.inputStream.bufferedReader().forEachLine { line ->
                    synchronized(out) { out.appendLine(line) }
                }
            } catch (_: Exception) { /* process destroyed */ }
        }.apply { isDaemon = true; start() }
        val finished = try {
            p.waitFor(timeoutSec, TimeUnit.SECONDS)
        } catch (_: InterruptedException) {
            false
        }
        if (!finished) {
            p.destroyForcibly()
            return false to "自检超时（${timeoutSec}s）"
        }
        reader.join(1500)
        val text = synchronized(out) { out.toString() }
        val ok = p.exitValue() == 0 && text.contains("selftest PASSED")
        return ok to if (ok) "CPU + Hexagon HMX 位精确校验通过" else text.takeLast(800)
    }

    fun start(cfg: MineConfig): Boolean {
        if (!running.compareAndSet(false, true)) return false
        if (!prepare()) {
            running.set(false)
            fail("挖矿组件缺失，请重新安装应用")
            return false
        }
        _state.value = MinerUiState(
            phase = MinePhase.STARTING,
            startedAtMs = System.currentTimeMillis(),
        )

        val script = try {
            writeScript(
                "run.sh",
                buildString {
                    append("cd '$nativeDir' || exit 41\n")
                    append("echo \$\$ > '${pidFile.absolutePath}'\n")
                    append(envExports())
                    append("export PRL_POOL_STATS_INTERVAL=1\n")
                    append("exec '${minerBin.absolutePath}' --npu")
                    append(" -o '${cfg.pool}' -u '${cfg.wallet}' -w '${cfg.effectiveWorker}'")
                    append(" --dialect '${cfg.dialect}'\n")
                },
            )
        } catch (e: Exception) {
            running.set(false)
            fail("启动失败，请重试")
            return false
        }

        val p = try {
            ProcessBuilder("sh", script.absolutePath).redirectErrorStream(true).start()
        } catch (e: Exception) {
            running.set(false)
            fail("启动失败，请重试")
            return false
        }
        proc = p
        // java.lang.Process.pid() is not available on this Android API level;
        // the runner script writes $$ (which survives exec) before launching the miner.
        Thread {
            var pid = -1
            for (i in 0 until 20) {
                pid = try {
                    pidFile.readText().trim().toIntOrNull() ?: -1
                } catch (_: Exception) { -1 }
                if (pid > 0) break
                try { Thread.sleep(50) } catch (_: InterruptedException) { break }
            }
            if (pid > 0) childPidListener?.invoke(pid)
        }.apply { isDaemon = true; start() }
        update { it.copy(phase = MinePhase.RUNNING) }

        readerThread = Thread {
            try {
                p.inputStream.bufferedReader().useLines { lines ->
                    for (line in lines) onLine(line)
                }
            } catch (_: Exception) { /* destroyed */ }
            childPidListener?.invoke(null)
            try { pidFile.delete() } catch (_: Exception) {}
            val userStopped = !running.getAndSet(false)
            val code = try {
                if (p.waitFor(2, TimeUnit.SECONDS)) p.exitValue() else -1
            } catch (_: Exception) {
                -1
            }
            val reason = when {
                userStopped -> _state.value.lastStopReason.ifEmpty { "已停止" }
                code == 0 -> "挖矿已停止"
                else -> "挖矿意外停止，请重试"
            }
            if (!userStopped && code != 0) addLog("miner exit code $code")
            update {
                it.copy(
                    phase = if (!userStopped && code != 0) MinePhase.ERROR else MinePhase.STOPPED,
                    lastStopReason = reason,
                )
            }
        }.apply { isDaemon = true; start() }
        return true
    }

    private val statsRe = Regex(
        """^\[(\d{2}:\d{2}:\d{2})]\s+([\d.]+) GMAC/s \| attempts (\d+) \| accepted (\d+) \| rejected (\d+)"""
    )

    private fun onLine(raw: String) {
        val line = raw.trimEnd()
        if (line.isEmpty()) return
        val m = statsRe.find(line)
        if (m != null) {
            val g = m.groupValues[2].toDoubleOrNull() ?: return
            val a = m.groupValues[3].toLongOrNull() ?: 0
            val acc = m.groupValues[4].toLongOrNull() ?: 0
            val rej = m.groupValues[5].toLongOrNull() ?: 0
            update {
                it.copy(
                    gmacs = g,
                    attempts = a,
                    accepted = acc,
                    rejected = rej,
                    history = (it.history + g).takeLast(MAX_HISTORY),
                )
            }
        } else if (line.contains("connected, authorize sent")) {
            update { it.copy(connected = true) }
            addLog(line)
        } else if (line.contains("cannot connect", true) ||
            line.contains("connect timeout", true) ||
            line.contains("pool connection lost", true)
        ) {
            update { it.copy(lastStopReason = "无法连接矿池，请检查地址和网络") }
            addLog(line)
        } else if (line.contains("rejected share", true) ||
            line.contains("fastrpc", true) ||
            line.contains("error", true) ||
            line.contains("HIT", true) ||
            line.contains("submit", true) ||
            line.contains("compound", true)
        ) {
            addLog(line)
        }
    }

    private fun fail(msg: String) {
        running.set(false)
        update { it.copy(phase = MinePhase.ERROR, lastStopReason = msg) }
    }

    fun stop(reason: String = "已停止") {
        if (!running.compareAndSet(true, false)) return
        update { it.copy(lastStopReason = reason) }
        val p = proc
        try { p?.destroy() } catch (_: Exception) {}
        Thread {
            try {
                if (p != null && !p.waitFor(2, TimeUnit.SECONDS)) p.destroyForcibly()
            } catch (_: Exception) {
                try { p?.destroyForcibly() } catch (_: Exception) {}
            }
            proc = null
        }.apply { isDaemon = true; start() }
        update { it.copy(phase = MinePhase.STOPPED) }
    }

    val isRunning: Boolean
        get() = running.get() && _state.value.phase == MinePhase.RUNNING

    private fun update(f: (MinerUiState) -> MinerUiState) {
        _state.value = f(_state.value)
    }

    private fun addLog(line: String) {
        update { it.copy(log = (it.log + line).takeLast(LOG_CAP)) }
    }

    companion object {
        const val MAX_HISTORY = 300 // 1 s stats -> 5 min window
        const val LOG_CAP = 14
        const val MINER = "libprlminer.so"
        const val FUSED_SKEL = "libprl_fused_fused_skel.so"
        const val COMPOUND_SKEL = "libprl_compound_batch_skel.so"
        const val GOLDEN = "golden.txt"
    }
}
