/* Signed 24-bit encoding is valid precisely when the lower bound is >= -2^23 and the upper bound is STRICTLY < 2^23. */
#ifndef PRL_FUSED_ADAPTIVE_EXPORT_H
#define PRL_FUSED_ADAPTIVE_EXPORT_H
#include <stdint.h>

static inline int prl_adaptive_s24_from_abs_sum(int64_t absolute_sum,
                                                int64_t signed_sum) {
    const int64_t upper=(255*absolute_sum-signed_sum)/2;
    const int64_t lower=-(255*absolute_sum+signed_sum)/2;
    return lower>=-INT64_C(8388608) && upper<INT64_C(8388608);
}

#ifndef PRL_ADAPTIVE_BOUNDS_ONLY
#ifndef PRL_ADAPTIVE_HOST_SIM
#include "hmx_read_fast.h"
#include "fused_fold_compact.h"

static inline void prl_hmx_read_corrected_bytes24_raw(uint8_t *vtcm,
                                                      uint32_t cfg2,
                                                      uint32_t scratch) {
    Q6_bias_mxmem2_A(vtcm+cfg2);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch,0);
    Q6_bias_mxmem2_A(vtcm+cfg2+256);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch+2048,0);
    Q6_bias_mxmem2_A(vtcm+cfg2+512);
    Q6_mxmem_AR_after_retain_cm_ub(vtcm+scratch+4096,0);
}

#endif /* !PRL_ADAPTIVE_HOST_SIM */

static void prl_compact_fold_raw24(int s,int cb,const uint8_t *raw,
                                  uint8_t *transcript_scratch) {
    const HVX_Vector *p0=(const HVX_Vector *)raw;
    const HVX_Vector *p1=(const HVX_Vector *)(raw+2048);
    const HVX_Vector *p2=(const HVX_Vector *)(raw+4096);
    HVX_Vector e0=Q6_V_vzero(),e1=Q6_V_vzero(),e2=Q6_V_vzero();
    HVX_Vector o0=Q6_V_vzero(),o1=Q6_V_vzero(),o2=Q6_V_vzero();
#pragma clang loop unroll(disable)
    for (int u=0;u<8;u++) {
        e0=Q6_V_vxor_VV(e0,p0[2*u]);o0=Q6_V_vxor_VV(o0,p0[2*u+1]);
        e1=Q6_V_vxor_VV(e1,p1[2*u]);o1=Q6_V_vxor_VV(o1,p1[2*u+1]);
        e2=Q6_V_vxor_VV(e2,p2[2*u]);o2=Q6_V_vxor_VV(o2,p2[2*u+1]);
    }
    const HVX_Vector zero=Q6_V_vzero();
    const HVX_Vector e3=Q6_V_vand_QR(Q6_Q_vcmp_gt_VbVb(zero,e2),-1);
    const HVX_Vector o3=Q6_V_vand_QR(Q6_Q_vcmp_gt_VbVb(zero,o2),-1);
    HVX_Vector a,b,c,d;
    prl_corrected_unpack4(e0,e1,e2,e3,&a,&b,&c,&d);
    const HVX_Vector low=prl_compact_four(a,b,c,d);
    prl_corrected_unpack4(o0,o1,o2,o3,&a,&b,&c,&d);
    const HVX_Vector high=prl_compact_four(a,b,c,d);
    HVX_Vector result=prl_compact_deal_lo(low,high,16);
    volatile HVX_Vector *dst=(volatile HVX_Vector *)transcript_scratch+(size_t)(cb/2)*16+s;
    if (cb&1) result=Q6_V_vxor_VV(result,*dst);
    *dst=result;
}
#endif
#endif
