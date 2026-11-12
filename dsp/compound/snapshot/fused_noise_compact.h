#ifndef PRL_FUSED_NOISE_COMPACT_H
#define PRL_FUSED_NOISE_COMPACT_H
#include <stdint.h>
#include <stddef.h>
#include "../compact_format.h"
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"
static inline HVX_Vector prl_noise_lookup128(HVX_Vector idx,HVX_Vector table) {
    HVX_Vector x=Q6_Vb_vlut32_VbVbI(idx,table,0);
    x=Q6_Vb_vlut32or_VbVbVbI(x,idx,table,1);
    x=Q6_Vb_vlut32or_VbVbVbI(x,idx,table,2);
    return Q6_Vb_vlut32or_VbVbVbI(x,idx,table,3);
}
static inline HVX_Vector prl_noise_expand_row(HVX_Vector base,HVX_Vector table,
                                              HVX_Vector p,HVX_Vector q,HVX_Vector sign) {
    const HVX_Vector delta=Q6_Vb_vsub_VbVb(prl_noise_lookup128(p,table),prl_noise_lookup128(q,table));
    return Q6_V_vxor_VV(Q6_Vb_vadd_VbVb(base,delta),sign);
}
/* SDK qhl HVX L2FETCH 2-D descriptor; requests 4 x 128 B with 2 KiB stride. */
static inline void prl_prefetch_rows(const void *addr) {
    const uint64_t control = HEXAGON_V64_CREATE_H(0,2048,128,4);
    __asm__ __volatile__("l2fetch(%0,%1)" : : "r"(addr), "r"(control));
}
static void prl_noise_make_block(const uint8_t *source,uint32_t rb,uint8_t *dest) {
    const unsigned attempt=rb/32u,row0=(rb%32u)*64u;
    const uint8_t *record=source+PRL_COMPACT_BASE_BYTES+(size_t)attempt*PRL_COMPACT_RECORD_BYTES;
    const uint8_t *p=record+PRL_COMPACT_P_OFFSET,*q=record+PRL_COMPACT_Q_OFFSET;
    const HVX_Vector sign=Q6_V_vsplat_R((int32_t)0x80808080u);
    for (unsigned s=0;s<16;s++) {
        const HVX_Vector ip=*(const HVX_Vector *)(p+(size_t)s*128u);
        const HVX_Vector iq=*(const HVX_Vector *)(q+(size_t)s*128u);
        HVX_Vector *d0=(HVX_Vector *)(dest+(size_t)s*8192u),*d1=d0+16,*d2=d0+32,*d3=d0+48;
#pragma clang loop unroll(disable)
        for (unsigned g=0;g<16;g++) {
            const unsigned r=row0+g*4u;
            if (g+4u<16u) prl_prefetch_rows(source+(size_t)(r+16u)*2048u+s*128u);
            HVX_Vector b0=*(const HVX_Vector *)(source+(size_t)(r+0)*2048u+s*128u);
            if (r==0&&s==0) {
                const HVX_Vector nonce=*(const HVX_Vector *)(record+PRL_COMPACT_NONCE_OFFSET);
                b0=Q6_V_vmux_QVV(Q6_Q_vsetq_R(8),nonce,b0);
            }
            const HVX_Vector a0=prl_noise_expand_row(b0,*(const HVX_Vector *)(record+(size_t)(r+0)*128u),ip,iq,sign);
            const HVX_Vector a1=prl_noise_expand_row(*(const HVX_Vector *)(source+(size_t)(r+1)*2048u+s*128u),*(const HVX_Vector *)(record+(size_t)(r+1)*128u),ip,iq,sign);
            const HVX_Vector a2=prl_noise_expand_row(*(const HVX_Vector *)(source+(size_t)(r+2)*2048u+s*128u),*(const HVX_Vector *)(record+(size_t)(r+2)*128u),ip,iq,sign);
            const HVX_Vector a3=prl_noise_expand_row(*(const HVX_Vector *)(source+(size_t)(r+3)*2048u+s*128u),*(const HVX_Vector *)(record+(size_t)(r+3)*128u),ip,iq,sign);
            const HVX_VectorPair p01=Q6_W_vshuff_VVR(a1,a0,-32),p23=Q6_W_vshuff_VVR(a3,a2,-32);
            const HVX_VectorPair low=Q6_W_vshuff_VVR(Q6_V_lo_W(p23),Q6_V_lo_W(p01),-64);
            const HVX_VectorPair high=Q6_W_vshuff_VVR(Q6_V_hi_W(p23),Q6_V_hi_W(p01),-64);
            d0[g]=Q6_V_lo_W(low);d1[g]=Q6_V_hi_W(low);d2[g]=Q6_V_lo_W(high);d3[g]=Q6_V_hi_W(high);
        }
    }
}
#endif
