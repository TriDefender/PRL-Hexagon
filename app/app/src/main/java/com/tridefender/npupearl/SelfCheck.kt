package com.tridefender.npupearl

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
            CheckItem("compound", "复合内核"),
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

        // 2. Resolve the SoC to a Hexagon target the APK carries a payload for.
        //    Each supported arch ships its own skeleton set (see DspArch);
        //    adding a future SoC is one table entry plus the payload build.
        item("platform") { it.copy(result = CheckResult.RUNNING) }
        val socModel = prop("ro.soc.model").ifBlank { Build.SOC_MODEL }
        val board = prop("ro.board.platform")
        val abiOk = Build.SUPPORTED_ABIS.any { it == "arm64-v8a" }
        val target = DspArch.forDevice(socModel, board)
        val supported = target != null && abiOk
        item("platform") {
            it.copy(
                result = if (supported) CheckResult.PASS else CheckResult.FAIL,
                detail = if (supported) "${target!!.label}，支持 NPU 加速"
                else "这台手机的型号暂不支持 NPU 挖矿",
            )
        }
        android.util.Log.i("PrlCheck", "soc model=$socModel board=$board abiOk=$abiOk target=${target?.arch}")
        if (!supported) return finish(st, onProgress)

        // 3. Hexagon NPU user-space pieces. 8 Elite exposes the per-domain
        //    /dev/fastrpc-cdsp node; some 8 Gen 3 kernels (e.g. OnePlus/
        //    ColorOS on SM8650) keep only the multiplexed /dev/adsprpc-smd
        //    node, which libcdsprpc.so also uses for CDSP sessions.
        item("hex") { it.copy(result = CheckResult.RUNNING) }
        val lib = listOf(
            "/vendor/lib64/libcdsprpc.so",
            "/system/lib64/libcdsprpc.so",
            "/odm/lib64/libcdsprpc.so",
        ).any { File(it).exists() }
        val dev = listOf(
            "/dev/fastrpc-cdsp",
            "/dev/cdsprpc-smd",
            "/dev/adsprpc-smd",
        ).any { File(it).exists() }
        val hexOk = lib && dev
        item("hex") {
            it.copy(
                result = if (hexOk) CheckResult.PASS else CheckResult.FAIL,
                detail = if (hexOk) "NPU 驱动可用" else "未找到 NPU 驱动，这台手机暂不支持",
            )
        }
        android.util.Log.i("PrlCheck", "fastrpc lib=$lib dev=$dev")
        if (!hexOk) return finish(st, onProgress)

        // 4. Payload: stage this arch's skeletons under the canonical FastRPC
        //    names and verify everything the miner needs is in place.
        item("payload") { it.copy(result = CheckResult.RUNNING) }
        val ready = miner.prepare() && miner.payloadReady
        item("payload") {
            it.copy(
                result = if (ready) CheckResult.PASS else CheckResult.FAIL,
                detail = if (ready) "挖矿组件完整（${miner.arch?.arch} 载荷已就绪）"
                else "挖矿组件缺失，请重新安装应用",
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

        // 6. Real compound-kernel check on the same bytefold module the miner
        //    uses.  The fused golden test above cannot catch a compound-only
        //    load/resource failure on another device.
        item("compound") { it.copy(result = CheckResult.RUNNING) }
        var (cok, cmsg) = miner.selftestCompound()
        if (!cok) {
            Thread.sleep(1200)
            val retry = miner.selftestCompound()
            cok = retry.first
            cmsg = retry.second
        }
        item("compound") {
            it.copy(
                result = if (cok) CheckResult.PASS else CheckResult.FAIL,
                detail = if (cok) "复合内核校验通过，这台手机可以挖矿"
                else "复合内核校验失败；这台手机可能不支持该内核，或组件版本不匹配",
            )
        }
        if (!cok) android.util.Log.i("PrlCheck", "compound raw output: $cmsg")

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
