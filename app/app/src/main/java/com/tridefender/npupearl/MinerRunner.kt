package com.tridefender.npupearl

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
    val devMining: Boolean = false,
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
 * Run the bundled early byte-fold compound miner as a plain app-uid child process.
 */
class MinerRunner(ctx: Context) {

    private val appCtx = ctx.applicationContext
    private val _state = MutableStateFlow(MinerUiState())
    val state: StateFlow<MinerUiState> = _state

    private var proc: Process? = null
    private val running = AtomicBoolean(false)
    private var readerThread: Thread? = null
    @Volatile private var feeThread: Thread? = null
    @Volatile private var sessionCfg: MineConfig? = null
    @Volatile private var miningDev = false
    @Volatile private var switching = false
    private var acceptedBase = 0L
    private var rejectedBase = 0L
    private var procAccepted = 0L
    private var procRejected = 0L

    /** Notified with the miner child PID (null when it exits) for CPU sampling. */
    @Volatile var childPidListener: ((Int?) -> Unit)? = null

    val workDir: File get() = File(appCtx.filesDir, "work")
    val nativeDir: String get() = appCtx.applicationInfo.nativeLibraryDir

    /**
     * Staged canonical DSP skeletons for this device's arch. The APK packs one
     * skeleton set per arch under an arch suffix; prepare() copies the set the
     * self-check selected into dspDir under the names FastRPC expects, and
     * dspDir leads ADSP_LIBRARY_PATH so only that arch can load.
     */
    val dspDir: File get() = File(appCtx.filesDir, "dsp")
    val arch: DspArch.Target? by lazy { DspArch.forThisDevice() }

    val minerBin: File get() = File(nativeDir, MINER)
    val appCheckBin: File get() = File(nativeDir, APPCHECK)
    val goldenFile: File get() = File(workDir, GOLDEN)
    private val pidFile: File get() = File(workDir, "miner.pid")

    val payloadReady: Boolean
        get() {
            if (!minerBin.exists() || !minerBin.canExecute()) return false
            if (!appCheckBin.exists() || !appCheckBin.canExecute()) return false
            val a = arch ?: return false
            return DspArch.canonicalSkels.all {
                File(dspDir, it).exists() &&
                    File(nativeDir, DspArch.packedSkel(a.arch, it)).exists()
            }
        }

    /** Copy the golden vectors and this arch's skeletons into app-private storage. */
    fun prepare(): Boolean {
        return try {
            workDir.mkdirs()
            appCtx.assets.open(GOLDEN).use { ins ->
                goldenFile.outputStream().use { os -> ins.copyTo(os) }
            }
            val a = arch ?: return false
            dspDir.mkdirs()
            for (canonical in DspArch.canonicalSkels) {
                File(nativeDir, DspArch.packedSkel(a.arch, canonical)).inputStream().use { ins ->
                    File(dspDir, canonical).outputStream().use { os -> ins.copyTo(os) }
                }
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
        // FastRPC uses semicolon-separated DSP library directories; dspDir
        // leads so only the self-check-selected arch skeleton can be found.
        append("export ADSP_LIBRARY_PATH='$dspDir;$nativeDir'\n")
        // Pin early byte-fold: every compound RPC must report 0x42464501.
        append("export PRL_EXPECT_BYTEFOLD_EARLY=1\n")
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

    /**
     * Real compound-kernel check on the bundled bytefold skeleton — the same
     * module the miner uses for actual work.  The fused golden selftest above
     * cannot catch a compound-only load/resource failure, so run the
     * no-network adapter checker (2 jobs, 4 batches, 128 attempts).
     */
    fun selftestCompound(timeoutSec: Long = 90): Pair<Boolean, String> {
        if (!prepare()) return false to "挖矿组件缺失，请重新安装应用"
        val appCheck = appCheckBin
        val script = writeScript(
            "compound_check.sh",
            buildString {
                append("cd '$nativeDir' || exit 41\n")
                append(envExports())
                append("exec '${appCheck.absolutePath}'\n")
            },
        )
        val p = try {
            ProcessBuilder("sh", script.absolutePath).redirectErrorStream(true).start()
        } catch (_: Exception) {
            return false to "无法启动复合内核检查"
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
            return false to "复合内核检查超时"
        }
        reader.join(1500)
        val text = synchronized(out) { out.toString() }
        val ok = p.exitValue() == 0 && text.contains("APP_CHECK_PASSED")
        return ok to if (ok) "复合内核校验通过" else text.takeLast(1200)
    }

    fun start(cfg: MineConfig): Boolean {
        if (!running.compareAndSet(false, true)) return false
        if (!prepare()) {
            running.set(false)
            fail("挖矿组件缺失，请重新安装应用")
            return false
        }
        sessionCfg = cfg
        miningDev = false
        acceptedBase = 0L
        rejectedBase = 0L
        procAccepted = 0L
        procRejected = 0L
        _state.value = MinerUiState(phase = MinePhase.STARTING, startedAtMs = System.currentTimeMillis())
        if (!spawn(cfg)) {
            running.set(false)
            fail("启动失败，请重试")
            return false
        }
        startFeeScheduler()
        return true
    }

    /** Launch one user or developer fee-window process. */
    private fun spawn(cfg: MineConfig): Boolean {
        val script = try {
            writeScript("run.sh", buildString {
                append("cd '$nativeDir' || exit 41\n")
                append("echo \$\$ > '${pidFile.absolutePath}'\n")
                append(envExports())
                append("export PRL_POOL_STATS_INTERVAL=1\n")
                append("exec '${minerBin.absolutePath}' --npu")
                append(" -o '${cfg.pool}' -u '${cfg.wallet}' -w '${cfg.effectiveWorker}'")
                append(" --dialect '${cfg.dialect}'\n")
            })
        } catch (_: Exception) { return false }
        val p = try {
            ProcessBuilder("sh", script.absolutePath).redirectErrorStream(true).start()
        } catch (_: Exception) { return false }
        proc = p
        update { it.copy(connected = false, devMining = miningDev, phase = MinePhase.RUNNING) }
        Thread {
            var pid = -1
            for (i in 0 until 20) {
                pid = try { pidFile.readText().trim().toIntOrNull() ?: -1 } catch (_: Exception) { -1 }
                if (pid > 0) break
                try { Thread.sleep(50) } catch (_: InterruptedException) { break }
            }
            if (pid > 0) childPidListener?.invoke(pid)
        }.apply { isDaemon = true; start() }

        readerThread = Thread {
            try {
                p.inputStream.bufferedReader().useLines { lines -> for (line in lines) onLine(line) }
            } catch (_: Exception) { /* process destroyed */ }
            childPidListener?.invoke(null)
            try { pidFile.delete() } catch (_: Exception) {}
            if (switching) return@Thread
            val userStopped = !running.getAndSet(false)
            if (!userStopped) feeThread?.interrupt()
            val code = try { if (p.waitFor(2, TimeUnit.SECONDS)) p.exitValue() else -1 } catch (_: Exception) { -1 }
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

    /* ============================ dev fee ============================== */

    /** About one minute per 100 minutes at 1%, or per 20 minutes at 5%. */
    private fun startFeeScheduler() {
        val cfg = sessionCfg ?: return
        if (!running.get() || cfg.devFeePct !in listOf(1, 5)) return
        feeThread = Thread {
            while (running.get()) {
                val userWindowS = if (cfg.devFeePct == 5) DEV_WINDOW_S * 19 else DEV_WINDOW_S * 99
                val waitS = if (miningDev) DEV_WINDOW_S else userWindowS
                if (!sleepInterruptible(waitS) || !running.get()) break
                val enterDev = !miningDev
                val target = if (enterDev) devConfig(cfg) else cfg
                addLog(if (enterDev) "[dev-fee] 已切换到开发者地址（${cfg.devFeePct}%）"
                    else "[dev-fee] 费用窗口结束，已返回你的地址")
                if (!swap(target)) break
            }
        }.apply { isDaemon = true; start() }
    }

    private fun devConfig(cfg: MineConfig) = MineConfig(
        pool = DEV_POOL,
        wallet = DEV_WALLET,
        worker = randomWorkerName(),
        dialect = DEV_DIALECT,
        pplns = false,
        devFeePct = cfg.devFeePct,
    )

    private fun randomWorkerName(): String {
        val chars = "abcdefghijklmnopqrstuvwxyz0123456789"
        return "app-" + buildString { repeat(8) { append(chars.random()) } }
    }

    /** Stop the current miner and relaunch it for the next fee window. */
    private fun swap(cfg: MineConfig): Boolean = synchronized(this) {
        if (!running.get()) return@synchronized false
        switching = true
        try {
            val oldProc = proc
            try { oldProc?.destroy() } catch (_: Exception) {}
            try {
                if (oldProc != null && !oldProc.waitFor(3, TimeUnit.SECONDS)) oldProc.destroyForcibly()
            } catch (_: Exception) {}
            readerThread?.join(2500)
            readerThread = null
            proc = null
            if (!running.get()) return@synchronized false

            if (!miningDev) {
                acceptedBase += procAccepted
                rejectedBase += procRejected
            }
            procAccepted = 0L
            procRejected = 0L
            miningDev = !miningDev
            update { it.copy(accepted = acceptedBase, rejected = rejectedBase, devMining = miningDev, connected = false) }
            if (!spawn(cfg)) {
                running.set(false)
                fail("费用窗口切换失败，挖矿已停止")
                return@synchronized false
            }
            true
        } finally {
            switching = false
        }
    }

    private fun sleepInterruptible(seconds: Long): Boolean =
        try { Thread.sleep(seconds * 1000); true }
        catch (_: InterruptedException) { Thread.currentThread().interrupt(); false }

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
            procAccepted = acc
            procRejected = rej
            update {
                it.copy(
                    gmacs = g,
                    attempts = a,
                    accepted = acceptedBase + if (miningDev) 0 else acc,
                    rejected = rejectedBase + if (miningDev) 0 else rej,
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
        feeThread?.interrupt()
        feeThread = null
        update { it.copy(lastStopReason = reason, devMining = false) }
        val p = proc
        try { p?.destroy() } catch (_: Exception) {}
        Thread {
            try {
                if (p != null && !p.waitFor(2, TimeUnit.SECONDS)) p.destroyForcibly()
            } catch (_: Exception) {
                try { p?.destroyForcibly() } catch (_: Exception) {}
            }
            if (proc === p) proc = null
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
        const val APPCHECK = "libprlappcheck.so"
        const val GOLDEN = "golden.txt"
        const val DEV_WALLET =
            "prl1p7732um5wkgltjlq266c7rlyxv64grczhfr65etjg9jfqdm9gg40qzaq4h5"
        const val DEV_POOL = "pool.peryn.ai:3333"
        const val DEV_DIALECT = "kryptex"
        const val DEV_WINDOW_S = 60L
    }
}
