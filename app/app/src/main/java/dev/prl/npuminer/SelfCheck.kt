package dev.prl.npuminer

import android.os.Build
import java.io.File

enum class CheckResult { PENDING, RUNNING, PASS, WARN, FAIL, SKIP }

data class CheckItem(
    val id: String,
    val title: String,
    val result: CheckResult = CheckResult.PENDING,
    val detail: String = "",
    val blocking: Boolean = true,
)

data class SelfCheckState(
    val running: Boolean = false,
    val done: Boolean = false,
    val passed: Boolean = false,
    val items: List<CheckItem> = defaultItems(),
) {
    companion object {
        fun defaultItems() = listOf(
            CheckItem("soc", "手机芯片"),
            CheckItem("platform", "手机型号"),
            CheckItem("hex", "NPU 加速"),
            CheckItem("payload", "挖矿组件"),
            CheckItem("golden", "计算校验"),
        )
    }
}

/**
 * A device is "supported" only if all blocking checks pass, including a real
 * FastRPC run of the bundled HMX skeleton that is compared bit-exactly
 * against the CPU golden vectors.
 */
class SelfCheck(private val miner: MinerRunner) {

    fun runAll(onProgress: (SelfCheckState) -> Unit): SelfCheckState {
        var st = SelfCheckState(running = true, items = SelfCheckState.defaultItems())

        fun emit(item: CheckItem) {
            st = st.copy(items = st.items.map { if (it.id == item.id) item else it })
            android.util.Log.i("PrlCheck", "${item.id} -> ${item.result} ${item.detail}")
            onProgress(st)
        }

        fun item(id: String, transform: (CheckItem) -> CheckItem) {
            emit(transform(st.items.first { it.id == id }))
        }

        // 1. Qualcomm SoC
        item("soc") { it.copy(result = CheckResult.RUNNING) }
        val socMfr = prop("ro.soc.manufacturer").ifBlank { Build.SOC_MANUFACTURER }
        val qualcomm = socMfr.contains("qti", true) || socMfr.contains("qualcomm", true)
        item("soc") {
            it.copy(
                result = if (qualcomm) CheckResult.PASS else CheckResult.FAIL,
                detail = if (qualcomm) "已检测到高通芯片" else "未检测到高通芯片，这台手机暂不支持",
            )
        }
        android.util.Log.i("PrlCheck", "soc manufacturer=$socMfr")
        if (!qualcomm) return finish(st, onProgress)

        // 2. Snapdragon 8 Elite / Hexagon v79 (the bundled skeleton is v79-only)
        item("platform") { it.copy(result = CheckResult.RUNNING) }
        val socModel = prop("ro.soc.model").ifBlank { Build.SOC_MODEL }
        val board = prop("ro.board.platform")
        val abiOk = Build.SUPPORTED_ABIS.any { it == "arm64-v8a" }
        val elite = (socModel.equals("SM8750", true) || board.equals("sun", true)) && abiOk
        item("platform") {
            it.copy(
                result = if (elite) CheckResult.PASS else CheckResult.FAIL,
                detail = if (elite) "Snapdragon 8 Elite，支持 NPU 加速"
                else "这台手机的型号暂不支持 NPU 挖矿",
            )
        }
        android.util.Log.i("PrlCheck", "soc model=$socModel board=$board abiOk=$abiOk")
        if (!elite) return finish(st, onProgress)

        // 3. Hexagon NPU user-space pieces.
        item("hex") { it.copy(result = CheckResult.RUNNING) }
        val lib = listOf(
            "/vendor/lib64/libcdsprpc.so",
            "/system/lib64/libcdsprpc.so",
            "/odm/lib64/libcdsprpc.so",
        ).any { File(it).exists() }
        val dev = File("/dev/fastrpc-cdsp").exists()
        val hexOk = lib && dev
        item("hex") {
            it.copy(
                result = if (hexOk) CheckResult.PASS else CheckResult.FAIL,
                detail = if (hexOk) "NPU 驱动可用" else "未找到 NPU 驱动，这台手机暂不支持",
            )
        }
        android.util.Log.i("PrlCheck", "fastrpc lib=$lib dev=$dev")
        if (!hexOk) return finish(st, onProgress)

        // 4. Payload
        item("payload") { it.copy(result = CheckResult.RUNNING) }
        val ready = miner.prepare() && miner.payloadReady
        item("payload") {
            it.copy(
                result = if (ready) CheckResult.PASS else CheckResult.FAIL,
                detail = if (ready) "挖矿组件完整" else "挖矿组件缺失，请重新安装应用",
            )
        }
        if (!ready) return finish(st, onProgress)

        // 5. Real FastRPC + HMX bit-exact selftest in the plain app domain.
        item("golden") { it.copy(result = CheckResult.RUNNING) }
        var (ok, msg) = miner.selftest()
        if (!ok) {
            // One retry: a concurrently running DSP session can transiently
            // fail buffer mapping; it should not masquerade as "unsupported".
            Thread.sleep(1200)
            val retry = miner.selftest()
            ok = retry.first
            msg = retry.second
        }
        item("golden") {
            it.copy(
                result = if (ok) CheckResult.PASS else CheckResult.FAIL,
                detail = if (ok) "计算校验通过，NPU 工作正常"
                else "计算校验失败，请重试；若仍失败，这台手机可能暂不支持",
            )
        }
        if (!ok) android.util.Log.i("PrlCheck", "golden raw output: $msg")

        return finish(st, onProgress)
    }

    private fun finish(st: SelfCheckState, onProgress: (SelfCheckState) -> Unit): SelfCheckState {
        val blockingFail = st.items.any { it.blocking && it.result == CheckResult.FAIL }
        val out = st.copy(running = false, done = true, passed = !blockingFail)
        onProgress(out)
        return out
    }

    private fun prop(name: String): String =
        try {
            Runtime.getRuntime().exec(arrayOf("getprop", name))
                .inputStream.bufferedReader().readText().trim()
        } catch (_: Exception) {
            ""
        }
}
