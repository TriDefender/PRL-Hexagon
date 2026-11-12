package dev.prl.npuminer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/**
 * The service exists so Android treats the process as foreground while mining
 * and does not freeze the DSP pipeline when the dashboard is not on screen.
 */
class MinerService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null
    private var updater: Thread? = null
    @Volatile private var updating = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification(0.0, 0, false)
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIF_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIF_ID, notification)
        }
        if (wakeLock == null) {
            val pm = getSystemService(PowerManager::class.java)
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PrlMiner:mining").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        startUpdater()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        updating = false
        updater?.interrupt()
        updater = null
        try { wakeLock?.release() } catch (_: Exception) {}
        wakeLock = null
        super.onDestroy()
    }

    /** Refresh the (fluid cloud) notification once per second while mining. */
    private fun startUpdater() {
        if (updating) return
        updating = true
        updater = Thread {
            val nm = getSystemService(NotificationManager::class.java)
            val miner = (application as PrlApp).graph.miner
            while (updating) {
                val st = miner.state.value
                if (!miner.isRunning) {
                    // MainActivity stops the service on phase changes; this is
                    // the backstop so no stale fluid cloud card remains.
                    stopSelf()
                    break
                }
                try {
                    nm.notify(NOTIF_ID, buildNotification(st.gmacs, st.accepted, st.connected))
                } catch (_: Exception) { /* channel gone during shutdown */ }
                try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
            }
        }.apply { isDaemon = true; start() }
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "挖矿状态", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "PRL 挖矿前台服务与流体云通知"
                    setShowBadge(false)
                },
            )
        }
    }

    private fun buildNotification(gmacs: Double, accepted: Long, connected: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val tmacs = gmacs / 1000.0
        val text = when {
            gmacs <= 0.0 && !connected -> "正在连接矿池…"
            gmacs <= 0.0 -> "正在等待算力数据…"
            else -> "算力 %.3f TMAC/s · 有效份额 %d".format(tmacs, accepted)
        }
        val builder = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_miner)
            .setContentTitle("PRL 挖矿")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(Notification.CATEGORY_SERVICE)

        if (gmacs > 0.0 && Build.VERSION.SDK_INT >= 36) {
            attachProgressStyle(builder, tmacs)
            // Capsule chip (≤7 chars): live hashrate like "7.08T".
            builder.setShortCriticalText("%.2fT".format(tmacs))
        }
        // Android 16 QPR1 (SDK 36.1+): ask ColorOS to promote this ongoing
        // notification into the 流体云 capsule.
        invokeQuietly(builder, "setRequestPromotedOngoing", java.lang.Boolean.TYPE, true)
        return builder.build()
    }

    /**
     * The bar maps the current hashrate onto a fixed 8 TMAC/s reference
     * (visual only, clamped at both ends).
     */
    private fun attachProgressStyle(builder: Notification.Builder, tmacs: Double) {
        val segment = Notification.ProgressStyle.Segment(100)
            .setColor(0xFF4D8DFF.toInt())
        val pct = (tmacs / REFERENCE_TMACS * 100.0).toInt().coerceIn(0, 100)
        val style = Notification.ProgressStyle()
            .setProgressSegments(listOf(segment))
            .setStyledByProgress(true)
            .setProgress(pct)
        builder.setStyle(style)
    }

    private fun invokeQuietly(
        target: Any,
        method: String,
        paramType: Class<*>,
        value: Any?,
    ) {
        // paramType is a resolved Class (e.g. java.lang.Boolean.TYPE); the
        // looked-up method may simply not exist on this SDK level.
        try {
            target.javaClass.getMethod(method, paramType).invoke(target, value)
        } catch (_: Throwable) {
            // Method only exists on newer SDKs (setShortCriticalText: 36,
            // setRequestPromotedOngoing: 36.1).
        }
    }

    companion object {
        private const val CHANNEL = "prl_mining"
        private const val NOTIF_ID = 1001

        /** Visual reference for the progress bar (≈ observed short-window peak). */
        private const val REFERENCE_TMACS = 8.0
    }
}
