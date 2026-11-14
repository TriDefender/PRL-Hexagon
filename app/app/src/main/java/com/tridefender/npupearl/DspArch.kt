package com.tridefender.npupearl

import android.os.Build

/**
 * Maps the device SoC to its Hexagon compute target. One APK bundles one DSP
 * payload set per supported SoC under a SoC-suffixed name (mirroring the
 * repo-root SM-prefixed payload folders); the self-check resolves this device
 * to one
 * SoC and MinerRunner stages that set under the canonical FastRPC names
 * before any RPC runs.
 *
 * Supporting a future SoC (8 Gen 5 / 8 Gen 5e class) is: build the payload
 * into its SM-prefixed folder (PRL_SOC=SMxxxx), add the SoC to build-apk.sh's
 * PAIRS, and add one entry to TARGETS — the suffix is the SoC, so two SoCs
 * sharing one Hexagon target can still carry differently tuned kernels.
 */
object DspArch {

    data class Target(
        val soc: String,
        val arch: String,
        val label: String,
        val board: String,
    )

    private val TARGETS = listOf(
        Target(soc = "SM8750", arch = "v79", label = "Snapdragon 8 Elite", board = "sun"),
        Target(soc = "SM8650", arch = "v75", label = "Snapdragon 8 Gen 3", board = "pineapple"),
        // Target(soc = "SM8850", arch = "v81", label = "Snapdragon 8 Gen 5", board = "..."),
    )

    /** Canonical FastRPC skeleton names the DSP loader expects (no suffix). */
    const val FUSED_SKEL = "libprl_fused_fused_skel.so"
    const val COMPOUND_SKEL = "libprl_compound_bytefold_batch_skel.so"

    val canonicalSkels = listOf(FUSED_SKEL, COMPOUND_SKEL)

    /** The bundled SoC-suffixed skeleton file for a canonical name. */
    fun packedSkel(soc: String, canonical: String): String =
        canonical.replaceSuffix(".so", "_$soc.so")

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
