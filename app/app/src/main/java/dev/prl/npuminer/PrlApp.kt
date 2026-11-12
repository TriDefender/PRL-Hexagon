package dev.prl.npuminer

import android.app.Application
import android.content.Context

/** Lives as long as the process (kept alive by the foreground service while mining). */
class AppGraph(ctx: Context) {
    val store = Store(ctx)
    val miner = MinerRunner(ctx)
    val monitor = SysMonitor(ctx)
    val check = SelfCheck(miner)

    init {
        miner.childPidListener = { pid -> monitor.minerPid = pid ?: -1 }
    }
}

class PrlApp : Application() {
    lateinit var graph: AppGraph
        private set

    override fun onCreate() {
        super.onCreate()
        graph = AppGraph(this)
    }
}
