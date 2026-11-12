/* output_mode 2: one bitmask u32/global_band; bit lane is digest_LE <= bound_LE. */
#ifndef PRL_FUSED_HASH_IMPL_H
#define PRL_FUSED_HASH_IMPL_H
#include <stdint.h>
#include <stddef.h>
#include "fused_blake3_hvx.h"

typedef struct {
    int output_mode;
    int compact_input;
    uint32_t keys[256];
    uint32_t bound[8];
} prl_hash_options;

/* Comparison is unsigned, most-significant-word first and includes equality at the boundary. */
static uint32_t prl_hash_hitmask32(const HVX_Vector digest[8],
                                  const uint32_t bound[8]) {
    const HVX_Vector zero=Q6_V_vzero();
    HVX_VectorPred equal=Q6_Q_vcmp_eq_VwVw(zero,zero);
    HVX_VectorPred less=Q6_Q_not_Q(equal);
#pragma clang loop unroll(disable)
    for (int word=7;word>=0;word--) {
        const HVX_Vector limit=Q6_V_vsplat_R((int32_t)bound[word]);
        const HVX_Vector d=digest[word];
        less=Q6_Q_or_QQ(less,Q6_Q_and_QQ(equal,Q6_Q_vcmp_gt_VuwVuw(limit,d)));
        equal=Q6_Q_and_QQ(equal,Q6_Q_vcmp_eq_VwVw(d,limit));
    }
    const HVX_VectorPred hit=Q6_Q_or_QQ(less,equal);
    static const uint32_t lane_bits[32] __attribute__((aligned(128)))={
        0x00000001u,0x00000002u,0x00000004u,0x00000008u,
        0x00000010u,0x00000020u,0x00000040u,0x00000080u,
        0x00000100u,0x00000200u,0x00000400u,0x00000800u,
        0x00001000u,0x00002000u,0x00004000u,0x00008000u,
        0x00010000u,0x00020000u,0x00040000u,0x00080000u,
        0x00100000u,0x00200000u,0x00400000u,0x00800000u,
        0x01000000u,0x02000000u,0x04000000u,0x08000000u,
        0x10000000u,0x20000000u,0x40000000u,0x80000000u};
    HVX_Vector bits=Q6_V_vand_QV(hit,*(const HVX_Vector *)lane_bits);
    bits=Q6_V_vor_VV(bits,Q6_V_vlalign_VVR(bits,bits,64));
    bits=Q6_V_vor_VV(bits,Q6_V_vlalign_VVR(bits,bits,32));
    bits=Q6_V_vor_VV(bits,Q6_V_vlalign_VVR(bits,bits,16));
    bits=Q6_V_vor_VV(bits,Q6_V_vlalign_VVR(bits,bits,8));
    bits=Q6_V_vor_VV(bits,Q6_V_vlalign_VVR(bits,bits,4));
    /* A single extraction per 32 hashes, rather than per tile or hash word. */
    return (uint32_t)Q6_R_vextract_VR(bits,0);
}

static void prl_hash_flush_compact(const prl_hash_options *options,uint32_t *out,
                                    int rb,int band,int nband,uint8_t *cum) {
    const size_t global_band=(size_t)rb*nband+band;
    const HVX_Vector *msg=(const HVX_Vector *)cum+(size_t)band*16;
    const uint32_t *key=options->keys+(rb/32)*8;
    if (options->output_mode==1) {
        prl_blake3_keyed_block64x32(msg,key,(HVX_Vector *)out+global_band*8);
    } else {
        /* Put each band's 1KiB digest temporary after ALL compact transcripts: workers own distinct bands and cannot race here. */
        HVX_Vector *digest=(HVX_Vector *)(cum+(size_t)nband*2048)+(size_t)band*8;
        prl_blake3_keyed_block64x32(msg,key,digest);
        out[global_band]=prl_hash_hitmask32(digest,options->bound);
    }
}
#endif
