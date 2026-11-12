/* Only use after the u8 offset correction has already been applied by HMX (bias export or the exact corrected-MM variant). */
#ifndef PRL_FUSED_FOLD_COMPACT_H
#define PRL_FUSED_FOLD_COMPACT_H

#include <stddef.h>
#include <stdint.h>
#include "fused_corrected.h"

#define PRL_COMPACT_BYTES_PER_BAND 2048u

static inline HVX_Vector prl_compact_deal_lo(HVX_Vector a,HVX_Vector b,int width) {
    return Q6_V_lo_W(Q6_W_vdeal_VVR(b,a,-width));
}

/* Four reduced row vectors have the four desired folds in even word lanes 0/2/4/6, repeated every eight words. */
static inline HVX_Vector prl_compact_four(HVX_Vector a,HVX_Vector b,
                                         HVX_Vector c,HVX_Vector d) {
    HVX_Vector ab=prl_compact_deal_lo(prl_corrected_reduce_columns(a),
                                    prl_corrected_reduce_columns(b),4);
    HVX_Vector cd=prl_compact_deal_lo(prl_corrected_reduce_columns(c),
                                    prl_corrected_reduce_columns(d),4);
    return prl_compact_deal_lo(ab,cd,16);
}

static void prl_compact_fold_raw(int s,int cb,const uint8_t *raw,
                                uint8_t *transcript_scratch) {
    const HVX_Vector *p0=(const HVX_Vector *)raw;
    const HVX_Vector *p1=(const HVX_Vector *)(raw+2048);
    const HVX_Vector *p2=(const HVX_Vector *)(raw+4096);
    const HVX_Vector *p3=(const HVX_Vector *)(raw+6144);
    HVX_Vector e0=Q6_V_vzero(),e1=Q6_V_vzero(),e2=Q6_V_vzero(),e3=Q6_V_vzero();
    HVX_Vector o0=Q6_V_vzero(),o1=Q6_V_vzero(),o2=Q6_V_vzero(),o3=Q6_V_vzero();
#pragma clang loop unroll(disable)
    for (int u=0;u<8;u++) {
        e0=Q6_V_vxor_VV(e0,p0[2*u]);o0=Q6_V_vxor_VV(o0,p0[2*u+1]);
        e1=Q6_V_vxor_VV(e1,p1[2*u]);o1=Q6_V_vxor_VV(o1,p1[2*u+1]);
        e2=Q6_V_vxor_VV(e2,p2[2*u]);o2=Q6_V_vxor_VV(o2,p2[2*u+1]);
        e3=Q6_V_vxor_VV(e3,p3[2*u]);o3=Q6_V_vxor_VV(o3,p3[2*u+1]);
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

/* Each resulting output vector contains all sixteen slice words of two adjacent tiles, matching the established TT format. */
static void prl_compact_flush_band(uint32_t *out,int rb,int band,int nband,
                                   const uint8_t *transcript_scratch) {
    const HVX_Vector *p=(const HVX_Vector *)transcript_scratch+(size_t)band*16;
    HVX_Vector a0=p[0],a1=p[1],a2=p[2],a3=p[3];
    HVX_Vector a4=p[4],a5=p[5],a6=p[6],a7=p[7];
    HVX_Vector a8=p[8],a9=p[9],a10=p[10],a11=p[11];
    HVX_Vector a12=p[12],a13=p[13],a14=p[14],a15=p[15];
#define PRL_COMPACT_TRANSPOSE(a,b,width) do { \
        HVX_VectorPair pair=Q6_W_vshuff_VVR((b),(a),-(width)); \
        (a)=Q6_V_lo_W(pair);(b)=Q6_V_hi_W(pair); \
    } while (0)
    PRL_COMPACT_TRANSPOSE(a0,a1,4);PRL_COMPACT_TRANSPOSE(a2,a3,4);
    PRL_COMPACT_TRANSPOSE(a4,a5,4);PRL_COMPACT_TRANSPOSE(a6,a7,4);
    PRL_COMPACT_TRANSPOSE(a8,a9,4);PRL_COMPACT_TRANSPOSE(a10,a11,4);
    PRL_COMPACT_TRANSPOSE(a12,a13,4);PRL_COMPACT_TRANSPOSE(a14,a15,4);
    PRL_COMPACT_TRANSPOSE(a0,a2,8);PRL_COMPACT_TRANSPOSE(a1,a3,8);
    PRL_COMPACT_TRANSPOSE(a4,a6,8);PRL_COMPACT_TRANSPOSE(a5,a7,8);
    PRL_COMPACT_TRANSPOSE(a8,a10,8);PRL_COMPACT_TRANSPOSE(a9,a11,8);
    PRL_COMPACT_TRANSPOSE(a12,a14,8);PRL_COMPACT_TRANSPOSE(a13,a15,8);
    PRL_COMPACT_TRANSPOSE(a0,a4,16);PRL_COMPACT_TRANSPOSE(a1,a5,16);
    PRL_COMPACT_TRANSPOSE(a2,a6,16);PRL_COMPACT_TRANSPOSE(a3,a7,16);
    PRL_COMPACT_TRANSPOSE(a8,a12,16);PRL_COMPACT_TRANSPOSE(a9,a13,16);
    PRL_COMPACT_TRANSPOSE(a10,a14,16);PRL_COMPACT_TRANSPOSE(a11,a15,16);
    PRL_COMPACT_TRANSPOSE(a0,a8,32);PRL_COMPACT_TRANSPOSE(a1,a9,32);
    PRL_COMPACT_TRANSPOSE(a2,a10,32);PRL_COMPACT_TRANSPOSE(a3,a11,32);
    PRL_COMPACT_TRANSPOSE(a4,a12,32);PRL_COMPACT_TRANSPOSE(a5,a13,32);
    PRL_COMPACT_TRANSPOSE(a6,a14,32);PRL_COMPACT_TRANSPOSE(a7,a15,32);
#undef PRL_COMPACT_TRANSPOSE
    HVX_Vector *dst=(HVX_Vector *)(out+((size_t)rb*nband+band)*512);
    dst[0]=a0;dst[1]=a8;dst[2]=a4;dst[3]=a12;
    dst[4]=a2;dst[5]=a10;dst[6]=a6;dst[7]=a14;
    dst[8]=a1;dst[9]=a9;dst[10]=a5;dst[11]=a13;
    dst[12]=a3;dst[13]=a11;dst[14]=a7;dst[15]=a15;
}

#endif
