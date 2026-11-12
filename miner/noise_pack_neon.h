/* Rank-128 A noise + direct HMX packing: A' = base + factor[p] - factor[q], then XOR 0x80 for unsigned input. */
#ifndef PRL_NOISE_PACK_NEON_H
#define PRL_NOISE_PACK_NEON_H

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "blake3_impl.h"

#if BLAKE3_USE_NEON != 1
#error "noise_pack_neon.h requires the official BLAKE3 NEON backend"
#endif

enum {
    PRL_NOISE_K = 2048,
    PRL_NOISE_RANK = 128,
    PRL_NOISE_BATCH = 32,
    PRL_NOISE_ROWS_BATCH = 8
};

static const uint8_t prl_noise_label_a[32] = {
    'A', '_', 't', 'e', 'n', 's', 'o', 'r'
};

typedef struct {
    uint32_t key[8];
    uint8_t p[PRL_NOISE_K];
    uint8_t q[PRL_NOISE_K];
} prl_noise_pack_state;

/* The BLAKE3 API supplies CHUNK_START and CHUNK_END separately; the counter
 * must stay zero for every independent input, rather than increment per input. */
static inline void prl_noise_hashes_neon(const uint32_t key[8],
        uint32_t first_index, unsigned count, unsigned slot, uint8_t *out) {
    uint8_t messages[PRL_NOISE_BATCH][64];
    const uint8_t *inputs[PRL_NOISE_BATCH];
    for (unsigned i = 0; i < count; ++i) {
        memset(messages[i], 0, 32);
        store32(messages[i] + 4 * slot, first_index + i + 1);
        memcpy(messages[i] + 32, prl_noise_label_a, 32);
        inputs[i] = messages[i];
    }
    blake3_hash_many_neon(inputs, count, 1, key, 0, false,
                         KEYED_HASH | ROOT, CHUNK_START, CHUNK_END, out);
}

/* Pair q deliberately uses the full u32 draw in mulhi, not just the seven low bits used by p. */
static inline void prl_noise_pack_init(prl_noise_pack_state *state,
                                      const uint8_t key[32]) {
    uint8_t hashes[PRL_NOISE_BATCH][32];
    load_key_words(key, state->key);
    for (unsigned g = 0; g < PRL_NOISE_K / 8; g += PRL_NOISE_BATCH) {
        prl_noise_hashes_neon(state->key, g, PRL_NOISE_BATCH, 1,
                             &hashes[0][0]);
        for (unsigned h = 0; h < PRL_NOISE_BATCH; ++h) {
            for (unsigned w = 0; w < 8; ++w) {
                const uint32_t x = load32(hashes[h] + 4 * w);
                const uint32_t p = x & 127;
                const unsigned l = (g + h) * 8 + w;
                state->p[l] = (uint8_t)p;
                state->q[l] = (uint8_t)(p ^ (1u +
                                  (uint32_t)(((uint64_t)127 * x) >> 32)));
            }
        }
    }
}

/* first_row is the absolute matrix row, so arbitrary disjoint worker ranges
 * preserve protocol indices. */
static inline void prl_noise_factors_neon(const prl_noise_pack_state *state,
        unsigned first_row, unsigned rows, uint8_t *factors) {
    prl_noise_hashes_neon(state->key, first_row * 4, rows * 4, 0, factors);
    const uint8x16_t mask = vdupq_n_u8(63), bias = vdupq_n_u8(32);
    for (unsigned i = 0; i < rows * PRL_NOISE_RANK; i += 16) {
        const uint8x16_t x = vld1q_u8(factors + i);
        vst1q_u8(factors + i, vsubq_u8(vandq_u8(x, mask), bias));
    }
}

/* For indices 0..127 exactly one table returns a nonzero candidate;
 * out-of-range indices return zero in the other table. */
static inline uint8x16_t prl_noise_lookup128(uint8x16x4_t lo,
        uint8x16x4_t hi, uint8x16_t index) {
    const uint8x16_t second = vsubq_u8(index, vdupq_n_u8(64));
    return vorrq_u8(vqtbl4q_u8(lo, index), vqtbl4q_u8(hi, second));
}

/* Output starts at the whole matrix's base, even when first_row is nonzero. */
static inline void prl_noise_apply_pack_nonce_neon(const prl_noise_pack_state *state,
        const int8_t *base, unsigned first_row, unsigned rows,
        const uint8_t *factors, const uint8_t *nonce8, uint8_t *packed) {
    const uint8x16_t flip = vdupq_n_u8(128);
    for (unsigned j = 0; j < rows; ++j) {
        const unsigned row = first_row + j;
        const uint8_t *fac = factors + (size_t)j * PRL_NOISE_RANK;
        uint8x16x4_t lo, hi;
        for (unsigned v = 0; v < 4; ++v) {
            lo.val[v] = vld1q_u8(fac + v * 16);
            hi.val[v] = vld1q_u8(fac + 64 + v * 16);
        }
        const uint8_t *arow = (const uint8_t *)base + (size_t)row * PRL_NOISE_K;
        uint8_t *band = packed + (size_t)(row / 64) * 64 * PRL_NOISE_K;
        for (unsigned s = 0; s < 16; ++s) {
            for (unsigned kt = 0; kt < 4; ++kt) {
                const unsigned l = s * 128 + kt * 32;
                uint8_t *dst = band + ((s * 4 + kt) * 64 + row % 64) * 32;
                for (unsigned half = 0; half < 32; half += 16) {
                    const unsigned k = l + half;
                    const uint8x16_t fp = prl_noise_lookup128(lo, hi,
                                                   vld1q_u8(state->p + k));
                    const uint8x16_t fq = prl_noise_lookup128(lo, hi,
                                                   vld1q_u8(state->q + k));
                    uint8x16_t a = vld1q_u8(arow + k);
                    if (nonce8 && row == 0 && k == 0)
                        a = vcombine_u8(vld1_u8(nonce8), vget_high_u8(a));
                    const uint8x16_t out = vaddq_u8(a, vsubq_u8(fp, fq));
                    vst1q_u8(dst + half, veorq_u8(out, flip));
                }
            }
        }
    }
}

static inline void prl_noise_apply_pack_neon(const prl_noise_pack_state *state,
        const int8_t *base, unsigned first_row, unsigned rows,
        const uint8_t *factors, uint8_t *packed) {
    prl_noise_apply_pack_nonce_neon(state, base, first_row, rows, factors,
                                    NULL, packed);
}

/* An optional nonce snapshot replaces base[0:8] without mutating base. */
static inline int prl_noise_pack_rows_nonce_neon(const prl_noise_pack_state *state,
        const int8_t *base, unsigned m, unsigned first_row, unsigned rows,
        const uint8_t *nonce8, uint8_t *packed) {
    if (!state || !base || !packed || !m || m % 64 ||
        m > UINT32_MAX / 4 || first_row > m || rows > m - first_row) return -1;
    uint8_t factors[PRL_NOISE_ROWS_BATCH][PRL_NOISE_RANK];
    for (unsigned done = 0; done < rows; done += PRL_NOISE_ROWS_BATCH) {
        const unsigned count = rows - done < PRL_NOISE_ROWS_BATCH ?
                               rows - done : PRL_NOISE_ROWS_BATCH;
        prl_noise_factors_neon(state, first_row + done, count, &factors[0][0]);
        prl_noise_apply_pack_nonce_neon(state, base, first_row + done, count,
                                  &factors[0][0], nonce8, packed);
    }
    return 0;
}

static inline int prl_noise_pack_rows_neon(const prl_noise_pack_state *state,
        const int8_t *base, unsigned m, unsigned first_row, unsigned rows,
        uint8_t *packed) {
    return prl_noise_pack_rows_nonce_neon(state, base, m, first_row, rows,
                                         NULL, packed);
}

static inline int prl_noise_pack_nonce_neon(const uint8_t key[32],
        const int8_t *base, unsigned m, const uint8_t *nonce8, uint8_t *packed) {
    if (!key || !base || !packed || !m || m % 64 || m > UINT32_MAX / 4) return -1;
    prl_noise_pack_state state;
    prl_noise_pack_init(&state, key);
    return prl_noise_pack_rows_nonce_neon(&state, base, m, 0, m, nonce8, packed);
}

static inline int prl_noise_pack_neon(const uint8_t key[32], const int8_t *base,
                                     unsigned m, uint8_t *packed) {
    return prl_noise_pack_nonce_neon(key, base, m, NULL, packed);
}

#endif
