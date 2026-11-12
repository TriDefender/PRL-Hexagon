/* For every K=128 slice, X=A+128 and C=bitwise_not(W)=-W-1 give X*W + 128*C + 16384 = A*W. */
#ifndef PRL_FUSED_CORRECTED_H
#define PRL_FUSED_CORRECTED_H

#include <stdint.h>
#include <string.h>
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"
#include "hexkl_micro.h"

/* WH and complement are 128-byte aligned; constant activation is 2048-aligned. */
static int prl_corrected_prepare(uint8_t *vtcm, uint32_t wh,
                                 uint32_t wh_bytes, uint32_t complement,
                                 uint32_t constant_activation,
                                 uint32_t constant_weight) {
    const HVX_Vector *w=(const HVX_Vector *)(vtcm+wh);
    HVX_Vector *cw=(HVX_Vector *)(vtcm+complement);
    const HVX_Vector ones=Q6_V_vsplat_R(-1);
    for (uint32_t i=0;i<wh_bytes/128;i++) cw[i]=Q6_V_vxor_VV(w[i],ones);
    HVX_Vector *a=(HVX_Vector *)(vtcm+constant_activation);
    const HVX_Vector a128=Q6_V_vsplat_R((int32_t)0x80808080u);
    for (int i=0;i<16;i++) a[i]=a128;
    int8_t w4[1024] __attribute__((aligned(128)));
    memset(w4,4,sizeof(w4));
    return hexkl_micro_hmx_rm_to_wh_i8(vtcm,constant_weight,w4,0,0,32);
}

static inline void prl_corrected_mm(uint8_t *vtcm, uint32_t act,
                                    uint32_t weight) {
    const uint8_t *a=vtcm+act,*w=vtcm+weight;
    __asm__ volatile("{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3) }"
                     :: "r"(a),"r"(w),"r"(31),"r"(896):"memory");
}

/* Keep the accumulator across slices; clear once before s0. */
static inline void prl_corrected_slice(uint8_t *vtcm, uint32_t act,
                                      uint32_t wh, uint32_t complement,
                                      uint32_t constant_activation,
                                      uint32_t constant_weight) {
    for (int k=0;k<4;k++) prl_corrected_mm(vtcm,act+k*2048,wh+k*1024);
    for (int k=0;k<4;k++) prl_corrected_mm(vtcm,constant_activation,complement+k*1024);
    prl_corrected_mm(vtcm,constant_activation,constant_weight);
}

static inline HVX_Vector prl_corrected_reduce_columns(HVX_Vector z) {
    z=Q6_V_vxor_VV(z,Q6_V_vlalign_VVR(z,z,64));
    z=Q6_V_vxor_VV(z,Q6_V_vlalign_VVR(z,z,32));
    return Q6_V_vxor_VV(z,Q6_V_valign_VVR(z,z,4));
}

/* Reconstruct four rows from four already XOR-reduced byte planes. */
static inline void prl_corrected_unpack4(HVX_Vector b0,HVX_Vector b1,
                                         HVX_Vector b2,HVX_Vector b3,
                                         HVX_Vector *r0,HVX_Vector *r1,
                                         HVX_Vector *r2,HVX_Vector *r3) {
    const HVX_VectorPair lo=Q6_W_vshuff_VVR(b1,b0,-1);
    const HVX_VectorPair hi=Q6_W_vshuff_VVR(b3,b2,-1);
    const HVX_VectorPair a=Q6_W_vshuff_VVR(Q6_V_lo_W(hi),Q6_V_lo_W(lo),-2);
    const HVX_VectorPair b=Q6_W_vshuff_VVR(Q6_V_hi_W(hi),Q6_V_hi_W(lo),-2);
    *r0=Q6_V_lo_W(a);*r1=Q6_V_hi_W(a);
    *r2=Q6_V_lo_W(b);*r3=Q6_V_hi_W(b);
}

/* Do not call this helper on uncorrected u8*i8 accumulators. */
static void prl_corrected_fold_raw(int s,int cb,const uint8_t *raw,
                                  uint8_t *transcript_scratch) {
    const HVX_Vector *p0=(const HVX_Vector *)(raw);
    const HVX_Vector *p1=(const HVX_Vector *)(raw+2048);
    const HVX_Vector *p2=(const HVX_Vector *)(raw+4096);
    const HVX_Vector *p3=(const HVX_Vector *)(raw+6144);
    HVX_Vector e0=Q6_V_vzero(),e1=Q6_V_vzero(),e2=Q6_V_vzero(),e3=Q6_V_vzero();
    HVX_Vector o0=Q6_V_vzero(),o1=Q6_V_vzero(),o2=Q6_V_vzero(),o3=Q6_V_vzero();
#pragma clang loop unroll(disable)
    for (int u=0;u<8;u++) {
        e0=Q6_V_vxor_VV(e0,p0[2*u]); o0=Q6_V_vxor_VV(o0,p0[2*u+1]);
        e1=Q6_V_vxor_VV(e1,p1[2*u]); o1=Q6_V_vxor_VV(o1,p1[2*u+1]);
        e2=Q6_V_vxor_VV(e2,p2[2*u]); o2=Q6_V_vxor_VV(o2,p2[2*u+1]);
        e3=Q6_V_vxor_VV(e3,p3[2*u]); o3=Q6_V_vxor_VV(o3,p3[2*u+1]);
    }
    HVX_Vector a,b,c,d;
    volatile HVX_Vector *dst=(volatile HVX_Vector *)transcript_scratch+(size_t)(cb/2)*128+s;
#define PRL_CORRECTED_SAVE(k,z) do { \
        HVX_Vector q=prl_corrected_reduce_columns(z); \
        if (cb&1) q=Q6_V_vxor_VV(q,dst[(k)*16]); \
        dst[(k)*16]=q; \
    } while (0)
    prl_corrected_unpack4(e0,e1,e2,e3,&a,&b,&c,&d);
    PRL_CORRECTED_SAVE(0,a);PRL_CORRECTED_SAVE(1,b);
    PRL_CORRECTED_SAVE(2,c);PRL_CORRECTED_SAVE(3,d);
    prl_corrected_unpack4(o0,o1,o2,o3,&a,&b,&c,&d);
    PRL_CORRECTED_SAVE(4,a);PRL_CORRECTED_SAVE(5,b);
    PRL_CORRECTED_SAVE(6,c);PRL_CORRECTED_SAVE(7,d);
#undef PRL_CORRECTED_SAVE
}

#endif
