/* Standalone consumer experiment: fold each exported byte plane spatially
 * before rebuilding int32 words. This file is not wired into a kernel.
 *
 * Requires an already corrected HMX accumulator and the frozen compact-fold
 * helpers. The 8-vector even/odd XORs and scratch/flush ABI match the baseline.
 */
#ifndef PRL_FUSED_BYTEFOLD_EARLY_H
#define PRL_FUSED_BYTEFOLD_EARLY_H

#include <stddef.h>
#include <stdint.h>
#include "fused_fold_compact.h"

static inline __attribute__((always_inline))
HVX_Vector prl_bytefold_early_plane(HVX_Vector even, HVX_Vector odd) {
    even=Q6_V_vxor_VV(even,Q6_V_valign_VVR(even,even,16));
    odd=Q6_V_vxor_VV(odd,Q6_V_valign_VVR(odd,odd,16));
    HVX_Vector z=prl_compact_deal_lo(even,odd,16);
    z=Q6_V_vxor_VV(z,Q6_V_valign_VVR(z,z,8));
    z=Q6_V_vxor_VV(z,Q6_V_valign_VVR(z,z,1));
    z=prl_compact_deal_lo(z,z,1);
    z=prl_compact_deal_lo(z,z,4);
    return z;
}

static inline __attribute__((always_inline))
HVX_Vector prl_bytefold_early_pack4(HVX_Vector c0,HVX_Vector c1,
                                   HVX_Vector c2,HVX_Vector c3) {
    const HVX_Vector lo=Q6_V_lo_W(Q6_W_vshuff_VVR(c1,c0,-1));
    const HVX_Vector hi=Q6_V_lo_W(Q6_W_vshuff_VVR(c3,c2,-1));
    return Q6_V_lo_W(Q6_W_vshuff_VVR(hi,lo,-2));
}

/* short_export is true only when the producer's existing signed-24 proof
 * authorizes a 3-plane export. Its branch does not load raw+6144. */
static inline __attribute__((always_inline))
void prl_bytefold_early_fold_raw(int s,int cb,const uint8_t *raw,
                                uint8_t *transcript_scratch,
                                int short_export) {
    const HVX_Vector *p0=(const HVX_Vector *)raw;
    const HVX_Vector *p1=(const HVX_Vector *)(raw+2048);
    const HVX_Vector *p2=(const HVX_Vector *)(raw+4096);
    HVX_Vector e0=Q6_V_vzero(),e1=Q6_V_vzero(),e2=Q6_V_vzero();
    HVX_Vector o0=Q6_V_vzero(),o1=Q6_V_vzero(),o2=Q6_V_vzero();
#pragma clang loop unroll(full)
    for (int u=0;u<8;u++) {
        e0=Q6_V_vxor_VV(e0,p0[2*u]); o0=Q6_V_vxor_VV(o0,p0[2*u+1]);
        e1=Q6_V_vxor_VV(e1,p1[2*u]); o1=Q6_V_vxor_VV(o1,p1[2*u+1]);
        e2=Q6_V_vxor_VV(e2,p2[2*u]); o2=Q6_V_vxor_VV(o2,p2[2*u+1]);
    }

    const HVX_Vector c0=prl_bytefold_early_plane(e0,o0);
    const HVX_Vector c1=prl_bytefold_early_plane(e1,o1);
    const HVX_Vector c2=prl_bytefold_early_plane(e2,o2);
    HVX_Vector c3;
    if (short_export) {
        c3=Q6_V_vand_QR(Q6_Q_vcmp_gt_VbVb(Q6_V_vzero(),c2),-1);
    } else {
        const HVX_Vector *p3=(const HVX_Vector *)(raw+6144);
        HVX_Vector e3=Q6_V_vzero(),o3=Q6_V_vzero();
#pragma clang loop unroll(full)
        for (int u=0;u<8;u++) {
            e3=Q6_V_vxor_VV(e3,p3[2*u]); o3=Q6_V_vxor_VV(o3,p3[2*u+1]);
        }
        c3=prl_bytefold_early_plane(e3,o3);
    }

    HVX_Vector result=prl_bytefold_early_pack4(c0,c1,c2,c3);
    volatile HVX_Vector *dst=(volatile HVX_Vector *)transcript_scratch+
                            (size_t)(cb/2)*16+s;
    if (cb&1) result=Q6_V_vxor_VV(result,*dst);
    *dst=result;
}

#endif
