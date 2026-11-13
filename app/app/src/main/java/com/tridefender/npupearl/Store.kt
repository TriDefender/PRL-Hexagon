package com.tridefender.npupearl

import android.content.Context
import android.content.SharedPreferences

data class MineConfig(
    val pool: String,
    val wallet: String,
    val worker: String,
    val dialect: String, // "object" (HeroMiners/LuckyPool) | "kryptex"
    val pplns: Boolean,
) {
    val effectiveWorker: String
        get() = if (pplns) "${worker}.pplns" else worker
}

/** Simple persistence for pool address, wallet, worker and scheme. */
class Store(ctx: Context) {
    private val sp: SharedPreferences =
        ctx.getSharedPreferences("prl_miner", Context.MODE_PRIVATE)

    fun load(): MineConfig = MineConfig(
        pool = sp.getString(KEY_POOL, "") ?: "",
        wallet = sp.getString(KEY_WALLET, "") ?: "",
        worker = sp.getString(KEY_WORKER, DEFAULT_WORKER) ?: DEFAULT_WORKER,
        dialect = sp.getString(KEY_DIALECT, DEFAULT_DIALECT) ?: DEFAULT_DIALECT,
        pplns = sp.getBoolean(KEY_PPLNS, false),
    )

    fun save(c: MineConfig) {
        sp.edit()
            .putString(KEY_POOL, c.pool)
            .putString(KEY_WALLET, c.wallet)
            .putString(KEY_WORKER, c.worker)
            .putString(KEY_DIALECT, c.dialect)
            .putBoolean(KEY_PPLNS, c.pplns)
            .apply()
    }

    companion object {
        private const val KEY_POOL = "pool"
        private const val KEY_WALLET = "wallet"
        private const val KEY_WORKER = "worker"
        private const val KEY_DIALECT = "dialect"
        private const val KEY_PPLNS = "pplns"

        // No pre-set pool or wallet: the user enters their own endpoint.
        const val DEFAULT_POOL = ""
        const val DEFAULT_WALLET = ""
        const val POOL_HINT = "example.miner.net:3333"
        const val WALLET_HINT = "prl1p..."
        const val DEFAULT_WORKER = "npu-app"
        const val DEFAULT_DIALECT = "object"
    }
}
