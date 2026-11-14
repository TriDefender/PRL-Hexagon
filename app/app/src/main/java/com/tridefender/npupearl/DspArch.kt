package com.tridefender.npupearl

import android.os.Build

/**
 * Maps the device SoC to its Hexagon compute target. One APK bundles one DSP
 * payload set per supported arch under an arch suffix; the self-check resolves
 * this device to one arch and MinerRunner stages that arch's skeletons under
 * the canonical FastRPC names before any RPC runs.
 *
 * Supporting a future SoC (8 Gen 5 / 8 Gen 5e class) is: build the payload
 * with PRL_DSP_ARCH=vNN, package the two _vNN skeletons (build-apk.sh), and
 * add one entry to TARGETS.
 */
object DspArch {

    data class Target(val arch: String, val label: String, val soc: String, val board: String)

    private val TARGETS = listOf(
        Target("v79", "Snapdragon 8 Elite", soc = "SM8750", board = "sun"),
        Target("v75", "Snapdragon 8 Gen 3", soc = "SM8650", board = "pineapple"),
        // Target("v81", "Snapdragon 8 Gen 5", soc = "SM8850", board = "..."),
    )

    /** Canonical FastRPC skeleton names the DSP loader expects (no arch suffix). */
    const val FUSED_SKEL = "libprl_fused_fused_skel.so"
    const val COMPOUND_SKEL = "libprl_compound_bytefold_batch_skel.so"

    val canonicalSkels = listOf(FUSED_SKEL, COMPOUND_SKEL)

    /** The bundled arch-suffixed skeleton file for a canonical name. */
    fun packedSkel(arch: String, canonical: String): String =
        canonical.replaceSuffix(".so", "_$arch.so")

    /** Resolve this device to a target, or null when the SoC is unsupported. */
    fun forDevice(socModel: String, board: String): Target? =
        TARGETS.firstOrNull { socModel.equals(it.soc, true) || board.equals(it.board, true) }

    fun forThisDevice(): Target? =
        forDevice(prop("ro.soc.model").ifBlank { Build.SOC_MODEL }, prop("ro.board.platform"))

    fun prop(name: String): String =
        try {
            Runtime.getRuntime().exec(arrayOf("getprop", name))
                .inputStream.bufferedReader().readText().trim()
        } catch (_: Exception) {
            ""
        }
}

private fun String.replaceSuffix(old: String, new: String): String =
    if (endsWith(old)) removeSuffix(old) + new else this
