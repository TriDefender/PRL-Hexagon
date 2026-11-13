/* Experimental V79 HMX accumulator readers.
 *
 * Build with -mv79 -mhmx -mhvx -mhvx-length=128B.  These helpers use the
 * compiler's HMX intrinsics; they do not acquire VTCM or the HMX lock.
 *
 * Required setup: hexkl_micro_hmx_setup_acc_read_int32(vtcm, cfg) succeeded.
 * cfg reserves the library's entire 16 KiB region, scratch is separate and
 * reserves at least 8 KiB, and both are 2048-byte aligned.  The caller must
 * keep scratch alive until all returned vectors have been consumed.
 *
 * The library's exact int32 reader uses FOUR retain byte exports, followed
 * by a separate HVX shuffle and an 8 KiB output write.  The byte helpers
 * below let a fused consumer use the reconstructed values in registers.
 * Halfword export variants are isolated in read_probe_impl.c. Device data
 * establish that 2x1 combines neighboring accumulators as odd+(even>>8),
 * so these variants cannot replace the lossless four-byte decoder.
 */
#ifndef PRL_HMX_READ_FAST_H
#define PRL_HMX_READ_FAST_H

#include <stdint.h>
#include <hexagon_types.h>
#include <hmx_hexagon_protos.h>
#include <hvx_hexagon_protos.h>

#ifndef __HMX__
#error "hmx_read_fast.h requires the Hexagon compiler option -mhmx"
#endif

/* This is the same bias configuration and instruction sequence as the
 * library int32 reader, with destinations redirected to caller scratch.
 * Plane order: [bits7:0][bits15:8][bits23:16][bits31:24], 2048 B each.
 * This order follows the library's scalar reference shuffle; the bias
 * words (0x6000, 0x4000, 0x2000, 0) are conversion configuration words,
 * not literal right-shift counts.
 * All exports retain the accumulator, permitting further MM operations.
 */
static inline void prl_hmx_read_bytes_raw(uint8_t *vtcm, uint32_t cfg,
                                         uint32_t scratch) {
    Q6_bias_mxmem_A(vtcm + cfg);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm + scratch, 0);
    Q6_bias_mxmem_A(vtcm + cfg + 0x400);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm + scratch + 2048, 0);
    Q6_bias_mxmem_A(vtcm + cfg + 0x800);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm + scratch + 4096, 0);
    Q6_bias_mxmem_A(vtcm + cfg + 0xc00);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm + scratch + 6144, 0);
}

/* Independent exact bias configuration, NOT the library-managed cfg region.
 * Each plane is 256 bytes: 32 conversion words, then 32 signed int32 column
 * biases. bias=mxmem2 loads both arrays; bias=mxmem discards the additive bias.
 *
 * read_bias_probe (2026-11-08, SM8750/v79) checked every one of 2048 outputs
 * against CPU for +1, -128*sum(W), and a nonuniform signed-column bias ramp.
 * All 32-channel cases passed; the 64-channel candidate failed every word.
 * Plane config values are the same as the library's int32 byte conversion.
 * The additive bias is applied before conversion, preserving exact carries.
 *
 * cfg2 and negative_correction must be 128-byte aligned and in VTCM.
 * cfg2 reserves exactly 1024 bytes and is additionally 256-byte aligned.
 * Call once per (slice, column-block32) after prefix correction is prepared;
 * a 16-slice n=1024 job uses 16*32*1024 = 512 KiB of configurations.
 */
#define PRL_HMX_BIAS32_CONFIG_BYTES 1024u
static inline void prl_hmx_prepare_bias32_config(uint8_t *cfg2,
                                                const int32_t *negative_correction) {
    const HVX_Vector bias=*(const HVX_Vector *)negative_correction;
    HVX_Vector *q=(HVX_Vector *)cfg2;
    q[0]=Q6_V_vsplat_R(0x6000); q[1]=bias;
    q[2]=Q6_V_vsplat_R(0x4000); q[3]=bias;
    q[4]=Q6_V_vsplat_R(0x2000); q[5]=bias;
    q[6]=Q6_V_vzero();          q[7]=bias;
}

/* Four exact corrected-byte planes in the same raw layout as read_bytes_raw.
 * The accumulator itself remains raw u8*i8 and is retained across slices.
 * Use the NEGATIVE PREFIX u8 correction for cumulative HMX state.
 * Bias tables are read-only during export and may be shared across attempts.
 */
static inline void prl_hmx_read_corrected_bytes_raw(uint8_t *vtcm,
                                                   uint32_t cfg2,
                                                   uint32_t scratch) {
    Q6_bias_mxmem2_A(vtcm+cfg2);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch,0);
    Q6_bias_mxmem2_A(vtcm+cfg2+256);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch+2048,0);
    Q6_bias_mxmem2_A(vtcm+cfg2+512);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch+4096,0);
    Q6_bias_mxmem2_A(vtcm+cfg2+768);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch+6144,0);
}

/* Read four successive rows (4 * 32 int32) from exported byte planes.
 * block is in [0,16); output row vectors correspond to rows 4*block+[0,3].
 * The library's scalar and HVX shuffle implementations establish this
 * byte-plane ordering.  The pair-shuffle semantics are V79 HVX PRM p300.
 */
static inline void prl_hmx_unpack_bytes4(const uint8_t *scratch,
                                        unsigned block,
                                        HVX_Vector *row0, HVX_Vector *row1,
                                        HVX_Vector *row2, HVX_Vector *row3) {
    const HVX_Vector h0 = *(const HVX_Vector *)(scratch + block * 128);
    const HVX_Vector h1 = *(const HVX_Vector *)(scratch + 2048 + block * 128);
    const HVX_Vector h2 = *(const HVX_Vector *)(scratch + 4096 + block * 128);
    const HVX_Vector h3 = *(const HVX_Vector *)(scratch + 6144 + block * 128);
    const HVX_VectorPair lo16 = Q6_W_vshuff_VVR(h1, h0, -1);
    const HVX_VectorPair hi16 = Q6_W_vshuff_VVR(h3, h2, -1);
    const HVX_VectorPair a = Q6_W_vshuff_VVR(Q6_V_lo_W(hi16), Q6_V_lo_W(lo16), -2);
    const HVX_VectorPair b = Q6_W_vshuff_VVR(Q6_V_hi_W(hi16), Q6_V_hi_W(lo16), -2);
    *row0 = Q6_V_lo_W(a);
    *row1 = Q6_V_hi_W(a);
    *row2 = Q6_V_lo_W(b);
    *row3 = Q6_V_hi_W(b);
}

#endif
