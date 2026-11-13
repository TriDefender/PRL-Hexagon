package com.tridefender.npupearl

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import java.io.File
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class MonitorState(
    val batteryTempC: Float = Float.NaN,
    val npuTempC: Float = Float.NaN,
    val cpuTempC: Float = Float.NaN,
    val cpuUsagePct: Float = Float.NaN,
    val memUsedPct: Float = Float.NaN,
    val powerW: Float = Float.NaN,
    val powerLabel: String = "电池功率",
)

/**
 * `socd` is intentionally NOT treated as a temperature: on SM8750 it is a BCL percentage sensor, not °C.
 */
class SysMonitor(ctx: Context) {

    private val appCtx = ctx.applicationContext
    private val _state = MutableStateFlow(MonitorState())
    val state: StateFlow<MonitorState> = _state

    @Volatile private var sampling = false
    private var thread: Thread? = null

    /** Miner child PID, set by AppGraph; used as a last-resort CPU source. */
    @Volatile var minerPid: Int = -1
    private var prevMinerTicks: Long = -1
    private var prevMinerAt: Long = 0

    private var npuZones: List<String> = emptyList()
    private var cpuZones: List<String> = emptyList()
    private var batteryLogSamples = 0
    private var zonesScanned = false
    private var prevCpu: LongArray? = null

    private fun readLines(f: String): List<String> =
        try { File(f).readLines().map { it.trim() } } catch (_: Exception) { emptyList() }

    private fun readLong(f: String): Long? =
        readLines(f).firstOrNull()?.toLongOrNull()

    private fun scanZones() {
        if (zonesScanned) return
        val zones = try {
            File("/sys/class/thermal").listFiles { f -> f.name.startsWith("thermal_zone") }?.toList()
        } catch (_: Exception) { null } ?: emptyList()
        val npu = mutableListOf<String>()
        val cpu = mutableListOf<String>()
        for (z in zones) {
            val type = readLines(z.resolve("type").absolutePath).firstOrNull()?.lowercase() ?: continue
            when {
                type.startsWith("nsp") -> npu.add(z.absolutePath)
                type.startsWith("cpu") -> cpu.add(z.absolutePath)
            }
        }
        npuZones = npu
        cpuZones = cpu
        zonesScanned = true
    }

    private fun zoneMax(zones: List<String>): Float {
        if (zones.isEmpty()) return Float.NaN
        val values = zones.mapNotNull {
            readLines("$it/temp").firstOrNull()?.toFloatOrNull()?.div(1000f)
        }
        return values.maxOrNull() ?: Float.NaN
    }

    fun sampleOnce(): MonitorState {
        scanZones()
        val (batTemp, watts, powerLabel) = readBattery()
        return MonitorState(
            batteryTempC = batTemp,
            npuTempC = zoneMax(npuZones),
            cpuTempC = zoneMax(cpuZones),
            cpuUsagePct = cpuUsage(),
            memUsedPct = memUsage(),
            powerW = watts,
            powerLabel = powerLabel,
        )
    }

    /**
     * Restrict the correction to the verified build; never infer units from the
     * magnitude of a sample.
     */
    private fun readBattery(): Triple<Float, Float, String> {
        val sticky = try {
            appCtx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        } catch (_: Exception) { null }
        val tempC = sticky?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            ?.takeIf { it != Int.MIN_VALUE }?.div(10f) ?: Float.NaN
        val mv = sticky?.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1)
            ?.takeIf { it > 0 }?.toLong()
        val bm = appCtx.getSystemService(BatteryManager::class.java)
        val rawCurrent = try {
            bm?.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
        } catch (_: Exception) { null }
        val validCurrent = rawCurrent?.takeUnless {
            it == Long.MIN_VALUE || it == Int.MIN_VALUE.toLong()
        }
        val milliampFirmware = Build.FINGERPRINT ==
            "OnePlus/PLQ110/OP6113L1:16/BP2A.250605.015/B.19fa10c_b26ef2_b26ef5:user/release-keys"
        val currentUa = validCurrent?.toDouble()?.times(if (milliampFirmware) 1000.0 else 1.0)
        // sysfs is denied to the app on this phone; do not treat a shell-only
        // source, an unsupported-property sentinel, or a missing voltage as zero.
        val watts = if (mv != null && currentUa != null) {
            kotlin.math.abs(mv.toDouble() * currentUa / 1e9).toFloat()
        } else Float.NaN
        val status = sticky?.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val label = when (status) {
            BatteryManager.BATTERY_STATUS_DISCHARGING -> "电池放电功率"
            BatteryManager.BATTERY_STATUS_CHARGING -> "电池充电功率"
            else -> "电池净功率"
        }
        if ((appCtx.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0 &&
            batteryLogSamples++ < 5) {
            android.util.Log.i("PrlBattery",
                "voltageMv=$mv rawCurrent=$rawCurrent unit=${if (milliampFirmware) "mA" else "uA"} watts=$watts")
        }
        return Triple(tempC, watts, label)
    }

    private fun cpuUsage(): Float {
        val cur = parseProcStat()
        if (cur != null) {
            val prev = prevCpu
            prevCpu = cur
            if (prev == null) return Float.NaN
            val dIdle = (cur[3] + cur[4]) - (prev[3] + prev[4])
            val dTotal = cur[7] - prev[7]
            if (dTotal <= 0) return Float.NaN
            val busy = dTotal - dIdle
            return (busy * 100f / dTotal).coerceIn(0f, 100f)
        }
        // Android 16 SELinux may deny /proc/stat to untrusted_app.
        val load = readLines("/proc/loadavg").firstOrNull()
            ?.split(Regex("\\s+"))?.getOrNull(0)?.toFloatOrNull()
        if (load != null) {
            val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
            return (load * 100f / cores).coerceIn(0f, 100f)
        }
        // This tracks load (and is readable without privileges) but is not a
        // true utilisation counter.
        val freq = cpuFreqLoad()
        if (!freq.isNaN()) return freq
        return minerCpuUsage()
    }

    private fun cpuFreqLoad(): Float {
        val policyDir = File("/sys/devices/system/cpu/cpufreq")
        val policies = try {
            policyDir.listFiles { f -> f.name.startsWith("policy") }
        } catch (_: Exception) { null } ?: return Float.NaN
        var cur = 0.0
        var max = 0.0
        for (p in policies) {
            val c = readLong("${p.absolutePath}/scaling_cur_freq") ?: continue
            val m = readLong("${p.absolutePath}/cpuinfo_max_freq") ?: continue
            cur += c.toDouble()
            max += m.toDouble()
        }
        if (max <= 0.0) return Float.NaN
        return (cur / max * 100.0).toFloat().coerceIn(0f, 100f)
    }

    /** CPU time of the miner child, expressed as % of all cores. */
    private fun minerCpuUsage(): Float {
        val pid = minerPid
        if (pid <= 0) return Float.NaN
        val stat = readLines("/proc/$pid/stat").firstOrNull() ?: return Float.NaN
        val close = stat.lastIndexOf(')')
        if (close < 0) return Float.NaN
        val fields = stat.substring(close + 1).trim().split(Regex("\\s+"))
        // fields[0] is state; utime is field 14 overall -> index 11 here.
        val utime = fields.getOrNull(11)?.toLongOrNull() ?: return Float.NaN
        val stime = fields.getOrNull(12)?.toLongOrNull() ?: 0L
        val cutime = fields.getOrNull(13)?.toLongOrNull() ?: 0L
        val cstime = fields.getOrNull(14)?.toLongOrNull() ?: 0L
        val ticks = utime + stime + cutime + cstime
        val now = System.nanoTime()
        val prev = prevMinerTicks
        val prevAt = prevMinerAt
        prevMinerTicks = ticks
        prevMinerAt = now
        if (prev < 0 || prevAt == 0L) return Float.NaN
        val dt = (now - prevAt) / 1e9
        if (dt <= 0.0) return Float.NaN
        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        // Android USER_HZ is 100 on arm64.
        return ((ticks - prev) / 100.0 / dt / cores * 100.0)
            .toFloat().coerceIn(0f, 100f)
    }

    /** [user, nice, system, idle, iowait, irq, softirq, total] */
    private fun parseProcStat(): LongArray? {
        val line = readLines("/proc/stat").firstOrNull() ?: return null
        val v = line.split(Regex("\\s+")).drop(1).mapNotNull { it.toLongOrNull() }
        if (v.size < 7) return null
        val total = v.take(7).sum()
        return longArrayOf(v[0], v[1], v[2], v[3], v.getOrElse(4) { 0 }, v[5], v[6], total)
    }

    private fun memUsage(): Float {
        val am = appCtx.getSystemService(ActivityManager::class.java)
        if (am != null) {
            try {
                val mi = ActivityManager.MemoryInfo()
                am.getMemoryInfo(mi)
                if (mi.totalMem > 0) {
                    return ((mi.totalMem - mi.availMem) * 100.0 / mi.totalMem)
                        .toFloat().coerceIn(0f, 100f)
                }
            } catch (_: Exception) { /* fall through */ }
        }
        val info = readLines("/proc/meminfo")
        val total = info.firstOrNull { it.startsWith("MemTotal") }
            ?.split(Regex("\\s+"))?.getOrNull(1)?.toLongOrNull()
        val avail = info.firstOrNull { it.startsWith("MemAvailable") }
            ?.split(Regex("\\s+"))?.getOrNull(1)?.toLongOrNull()
        if (total == null || avail == null || total == 0L) return Float.NaN
        return ((total - avail) * 100f / total).coerceIn(0f, 100f)
    }

    fun start(periodMs: Long = 1000) {
        if (sampling) return
        sampling = true
        thread = Thread {
            while (sampling) {
                val s = try { sampleOnce() } catch (_: Exception) { MonitorState() }
                _state.value = s
                try { Thread.sleep(periodMs) } catch (_: InterruptedException) { break }
            }
        }.apply { isDaemon = true; start() }
    }

    fun stop() {
        sampling = false
        thread?.interrupt()
        thread = null
    }
}
