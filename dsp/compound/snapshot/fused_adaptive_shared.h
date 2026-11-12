/* The producer must pass short_export only when every corrected int32 value in this 64x32 snapshot fits signed24 (the cached weight-prefix proof). */
#ifndef PRL_FUSED_ADAPTIVE_SHARED_H
#define PRL_FUSED_ADAPTIVE_SHARED_H
#include "fused_fold_compact.h"

static inline __attribute__((always_inline))
void prl_compact_fold_adaptive_shared(int s,int cb,const uint8_t *raw,
                                     uint8_t *transcript_scratch,int short_export) {
    const HVX_Vector *p0=(const HVX_Vector *)raw;
    const HVX_Vector *p1=(const HVX_Vector *)(raw+2048);
    const HVX_Vector *p2=(const HVX_Vector *)(raw+4096);
    HVX_Vector e0=Q6_V_vzero(),e1=Q6_V_vzero(),e2=Q6_V_vzero();
    HVX_Vector o0=Q6_V_vzero(),o1=Q6_V_vzero(),o2=Q6_V_vzero();
#pragma clang loop unroll(full)
    for (int u=0;u<8;u++) {
        e0=Q6_V_vxor_VV(e0,p0[2*u]);o0=Q6_V_vxor_VV(o0,p0[2*u+1]);
        e1=Q6_V_vxor_VV(e1,p1[2*u]);o1=Q6_V_vxor_VV(o1,p1[2*u+1]);
        e2=Q6_V_vxor_VV(e2,p2[2*u]);o2=Q6_V_vxor_VV(o2,p2[2*u+1]);
    }
    HVX_Vector e3,o3;
    if (short_export) {
        const HVX_Vector zero=Q6_V_vzero();
        e3=Q6_V_vand_QR(Q6_Q_vcmp_gt_VbVb(zero,e2),-1);
        o3=Q6_V_vand_QR(Q6_Q_vcmp_gt_VbVb(zero,o2),-1);
    } else {
        const HVX_Vector *p3=(const HVX_Vector *)(raw+6144);
        e3=Q6_V_vzero();o3=Q6_V_vzero();
#pragma clang loop unroll(full)
        for (int u=0;u<8;u++) {
            e3=Q6_V_vxor_VV(e3,p3[2*u]);o3=Q6_V_vxor_VV(o3,p3[2*u+1]);
        }
    }
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
