#ifndef PRL_NOISE_COMPACT_NEON_H
#define PRL_NOISE_COMPACT_NEON_H
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
#include "compact_format.h"
#include "noise_pack_neon.h"
/* Builds one immutable nonce record; caller owns base and records. */
static inline int prl_noise_compact_record(const uint8_t key[32],const uint8_t nonce[8],uint8_t *record) {
    if (!key||!nonce||!record) return -1;
    prl_noise_pack_state state;
    prl_noise_pack_init(&state,key);
    memcpy(record+PRL_COMPACT_P_OFFSET,state.p,2048);
    memcpy(record+PRL_COMPACT_Q_OFFSET,state.q,2048);
    memset(record+PRL_COMPACT_NONCE_OFFSET,0,128);
    memcpy(record+PRL_COMPACT_NONCE_OFFSET,nonce,8);
    uint8_t factors[8][128];
    for (unsigned row=0;row<2048;row+=8) {
        prl_noise_factors_neon(&state,row,8,&factors[0][0]);
        for (unsigned j=0;j<8;j++) for (unsigned k=0;k<64;k+=16) {
            const uint8x16_t lo=vld1q_u8(factors[j]+k),hi=vld1q_u8(factors[j]+64+k);
            uint8_t *dst=record+(size_t)(row+j)*128u+2u*k;
            vst1q_u8(dst,vzip1q_u8(lo,hi));vst1q_u8(dst+16,vzip2q_u8(lo,hi));
        }
    }
    return 0;
}
#endif
