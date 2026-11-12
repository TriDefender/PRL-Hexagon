/* These helpers use the compiler's HMX intrinsics; they do not acquire VTCM or the HMX lock. */
#ifndef PRL_HMX_READ_FAST_H
#define PRL_HMX_READ_FAST_H

#include <stdint.h>
#include <hexagon_types.h>
#include <hmx_hexagon_protos.h>
#include <hvx_hexagon_protos.h>

#ifndef __HMX__
#error "hmx_read_fast.h requires the Hexagon compiler option -mhmx"
#endif

/* Plane order: [bits7:0][bits15:8][bits23:16][bits31:24], 2048 B each. */
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

/* cfg2 and negative_correction must be 128-byte aligned and in VTCM. */
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

/* The accumulator itself remains raw u8*i8 and is retained across slices. */
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

/* The pair-shuffle semantics are V79 HVX PRM p300. */
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
