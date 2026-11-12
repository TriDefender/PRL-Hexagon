/* Destination: X[slice16][kt4][row64][k32], uint8 A+128, 2048-byte aligned. */
#ifndef PRL_FUSED_SIGNED_PACK_H
#define PRL_FUSED_SIGNED_PACK_H

#include <stddef.h>
#include <stdint.h>
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"

#define PRL_SIGNED_PACK_BLOCK_BYTES (16u * 64u * 128u)

static void prl_pack_signed_rm_block(uint8_t *dst,const int8_t *A,int m,int rb) {
    const HVX_Vector sign=Q6_V_vsplat_R((int32_t)0x80808080u);
    for (int s=0;s<16;s++) {
        const HVX_Vector *src=(const HVX_Vector *)(A+((size_t)s*m+(size_t)rb*64)*128);
        HVX_Vector *d0=(HVX_Vector *)(dst+(size_t)s*8192);
        HVX_Vector *d1=d0+16,*d2=d0+32,*d3=d0+48;
#pragma clang loop unroll(disable)
        for (int g=0;g<16;g++) {
            const HVX_Vector a0=Q6_V_vxor_VV(src[g*4],sign);
            const HVX_Vector a1=Q6_V_vxor_VV(src[g*4+1],sign);
            const HVX_Vector a2=Q6_V_vxor_VV(src[g*4+2],sign);
            const HVX_Vector a3=Q6_V_vxor_VV(src[g*4+3],sign);
            const HVX_VectorPair p01=Q6_W_vshuff_VVR(a1,a0,-32);
            const HVX_VectorPair p23=Q6_W_vshuff_VVR(a3,a2,-32);
            const HVX_VectorPair low=Q6_W_vshuff_VVR(Q6_V_lo_W(p23),Q6_V_lo_W(p01),-64);
            const HVX_VectorPair high=Q6_W_vshuff_VVR(Q6_V_hi_W(p23),Q6_V_hi_W(p01),-64);
            d0[g]=Q6_V_lo_W(low); d1[g]=Q6_V_hi_W(low);
            d2[g]=Q6_V_lo_W(high);d3[g]=Q6_V_hi_W(high);
        }
    }
}

#endif
