/*
 * Work layout per attempt (m rows of A', n rows of B'ᵀ, k columns, r = 128):
 *   for each 64-row band of A' (holds 8 tile row-bases; 4 col-bases per 64 cols):
 *     for each slice s in 0..k/r:
 *       acc_band[i][j] += dot(A'[band_i][slice], B'ᵀ[j][slice])      (i32)
 *       fold each tile: XOR of its 128 cumulative acc cells
 *         -> t[s mod 16] = rotl(t[s mod 16], 13) ^ fold
 *     after the last slice: digest = blake3_keyed(a_noise_seed, t) per tile;
 *     hit when the little-endian u256 digest <= share bound.
 */
#define _GNU_SOURCE
#include "pearl.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

typedef int32_t (*dot128_fn)(const int16_t *a, const int16_t *b);

#if defined(__x86_64__)
__attribute__((target("avx2"))) static int32_t dot128_avx2(const int16_t *a, const int16_t *b) {
    __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
    for (unsigned off = 0; off < 128; off += 32) {
        s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(a + off)),
                                                    _mm256_loadu_si256((const __m256i *)(b + off))));
        s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(_mm256_loadu_si256((const __m256i *)(a + off + 16)),
                                                    _mm256_loadu_si256((const __m256i *)(b + off + 16))));
    }
    __m256i s = _mm256_add_epi32(s0, s1);
    __m128i lo = _mm256_castsi256_si128(s), hi = _mm256_extracti128_si256(s, 1);
    __m128i sum = _mm_add_epi32(lo, hi);
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(_mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2))));
}
#endif

static int32_t dot128_scalar(const int16_t *a, const int16_t *b) {
    int64_t acc = 0; /* exact: 128 * 127 * 127 << 2^31 */
    for (unsigned l = 0; l < 128; l++) acc += (int32_t)a[l] * (int32_t)b[l];
    return (int32_t)acc;
}

static dot128_fn g_dot128 = dot128_scalar;

void pr_dot_dispatch(void) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) g_dot128 = dot128_avx2;
#endif
}

struct pr_thread_ws {
    pr_attempt_mats am;
    int32_t *acc;      /* 64 * n band accumulators */
    uint32_t *tt;      /* transcripts of the current band: 8*4 col-bases * 16 words */
    pr_commitment c;
    uint8_t header[76];
    uint8_t target_be[32];
    pr_u256 bound;
    char job_id[80];
    bool bound_ok;
};

struct pr_ctx {
    uint32_t m, n, k;
    unsigned n_threads;
    struct pr_thread_ws *ws;
    atomic_uint_fast64_t *macs; /* per-thread credited-MAC counter */
};

pr_ctx *pr_ctx_new(uint32_t m, uint32_t n, uint32_t k, unsigned n_threads) {
    pr_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->m = m; ctx->n = n; ctx->k = k; ctx->n_threads = n_threads;
    ctx->ws = calloc(n_threads, sizeof(struct pr_thread_ws));
    ctx->macs = calloc(n_threads, sizeof(atomic_uint_fast64_t));
    if (!ctx->ws || !ctx->macs) abort();
    for (unsigned t = 0; t < n_threads; t++) {
        struct pr_thread_ws *w = &ctx->ws[t];
        w->am.m = m; w->am.n = n; w->am.k = k;
        w->am.a = malloc((size_t)m * k);
        w->am.bt = malloc((size_t)n * k);
        w->am.ap = malloc((size_t)m * k);
        w->am.btp = malloc((size_t)n * k);
        w->am.a16 = malloc(sizeof(int16_t) * (size_t)m * k);
        w->am.bt16 = malloc(sizeof(int16_t) * (size_t)n * k);
        w->acc = malloc(sizeof(int32_t) * 64 * n);
        w->tt = malloc(sizeof(uint32_t) * PEARL_ROW_BASES_PER_BAND * PEARL_COL_BASES_PER_BAND *
                       PEARL_TRANSCRIPT_WORDS);
        if (!w->am.a || !w->am.bt || !w->am.ap || !w->am.btp || !w->am.a16 || !w->am.bt16 || !w->acc || !w->tt)
            abort();
    }
    return ctx;
}

void pr_ctx_free(pr_ctx *ctx) {
    if (!ctx) return;
    for (unsigned t = 0; t < ctx->n_threads; t++) {
        struct pr_thread_ws *w = &ctx->ws[t];
        free(w->am.a); free(w->am.bt); free(w->am.ap); free(w->am.btp);
        free(w->am.a16); free(w->am.bt16); free(w->acc); free(w->tt);
    }
    free(ctx->ws); free(ctx->macs); free(ctx);
}

uint64_t pr_ctx_macs(const pr_ctx *ctx, unsigned thread_id) {
    return atomic_load_explicit(&ctx->macs[thread_id], memory_order_relaxed);
}

static void job_snapshot(struct pr_thread_ws *w, const uint8_t header76[76], const uint8_t target_be[32],
                         const char *job_id) {
    memcpy(w->header, header76, 76);
    memcpy(w->target_be, target_be, 32);
    snprintf(w->job_id, sizeof(w->job_id), "%s", job_id);
    w->bound_ok = pr_share_bound(target_be, w->am.k, &w->bound);
}

/* one band: full slice loop (GEMM + folds), leaving w->tt with final transcripts */
static void run_band(struct pr_thread_ws *w, uint32_t band) {
    const uint32_t n = w->am.n, k = w->am.k;
    const unsigned r = PEARL_RANK;
    const uint32_t slices = k / r;

    memset(w->acc, 0, sizeof(int32_t) * 64 * n);
    memset(w->tt, 0, sizeof(uint32_t) * PEARL_ROW_BASES_PER_BAND * PEARL_COL_BASES_PER_BAND *
                         PEARL_TRANSCRIPT_WORDS);
    for (uint32_t s = 0; s < slices; s++) {
        const size_t off = (size_t)s * r;
        for (uint32_t i = 0; i < 64; i++) {
            const int16_t *ar = w->am.a16 + (size_t)(band * 64 + i) * k + off;
            int32_t *accrow = w->acc + (size_t)i * n;
            for (uint32_t j = 0; j < n; j++) {
                const int16_t *br = w->am.bt16 + (size_t)j * k + off;
                accrow[j] += g_dot128(ar, br);
            }
        }

        /* fold every tile of the band against the cumulative accumulators */
        uint32_t s16 = s % PEARL_TRANSCRIPT_WORDS;
        for (uint32_t rb = 0; rb < PEARL_ROW_BASES_PER_BAND; rb++) {
            for (uint32_t cb = 0; cb < PEARL_COL_BASES_PER_BAND; cb++) {
                uint32_t t_cols = cb * 2; /* valid col bases inside a 64-band: 0,2,4,6 */
                uint32_t fold = 0;
                for (unsigned u = 0; u < PEARL_TILE_ROWS; u++) {
                    const int32_t *accrow = w->acc + (size_t)(rb + pr_row_off(u)) * n;
                    for (unsigned v = 0; v < PEARL_TILE_COLS; v++) {
                        fold ^= (uint32_t)accrow[t_cols + pr_col_off(v)];
                    }
                }
                size_t ti = ((size_t)rb * PEARL_COL_BASES_PER_BAND + cb) * PEARL_TRANSCRIPT_WORDS;
                uint32_t *tp = w->tt + ti;
                tp[s16] = pr_rotl32(tp[s16], PEARL_TRANSCRIPT_ROTL) ^ fold;
            }
        }
    }
}

bool pr_attempt(pr_ctx *ctx, unsigned thread_id, const uint8_t header76[76], const uint8_t target_be[32],
                const char *job_id, uint64_t seed, pr_share *share, uint32_t *hit_t_rows,
                uint32_t *hit_t_cols, uint64_t *macs_done) {
    struct pr_thread_ws *w = &ctx->ws[thread_id];
    const uint32_t m = ctx->m, n = ctx->n, k = ctx->k;
    const uint32_t bands = m / 64;

    job_snapshot(w, header76, target_be, job_id);
    if (!w->bound_ok) {
        if (macs_done) *macs_done = 0;
        return false;
    }

    pr_fill_base(&w->am, seed);
    pr_commit(w->header, k, w->am.a, w->am.bt, m, n, &w->c);
    pr_noise_operands(&w->am, &w->c);

    uint64_t macs = 0;
    bool hit = false;
    uint32_t hb_r = 0, hb_c = 0;

    for (uint32_t band = 0; band < bands && !hit; band++) {
        run_band(w, band);
        macs += (uint64_t)64 * n * (k - k % PEARL_RANK);
        /* transcript complete for this band: one digest per tile */
        for (uint32_t rb = 0; rb < PEARL_ROW_BASES_PER_BAND && !hit; rb++) {
            for (uint32_t cb = 0; cb < PEARL_COL_BASES_PER_BAND; cb++) {
                size_t ti = ((size_t)rb * PEARL_COL_BASES_PER_BAND + cb) * PEARL_TRANSCRIPT_WORDS;
                uint8_t digest[32];
                pr_blake3_keyed_block64(w->c.a_noise_seed.b, (const uint8_t *)(w->tt + ti), digest);
                pr_u256 d;
                memcpy(d.w, digest, 32); /* U256::from_little_endian */
                if (pr_u256_cmp(&d, &w->bound) <= 0) {
                    hit = true;
                    hb_r = band * 64 + rb;
                    hb_c = cb * 2;
                    break;
                }
            }
        }
    }

    atomic_fetch_add_explicit(&ctx->macs[thread_id], macs, memory_order_relaxed);
    if (macs_done) *macs_done = macs;
    if (!hit) return false;

    if (hit_t_rows) *hit_t_rows = hb_r;
    if (hit_t_cols) *hit_t_cols = hb_c;
    if (share && !pr_build_share(&w->am, &w->c, hb_r, hb_c, share)) return false;
    return true;
}

/* ---- kernel selftest: band 0 through the real kernel vs scalar tiles ---- */

static int g_band_selftest_ok;

int pr_band_selftest_ok(void) { return g_band_selftest_ok; }

void pr_band_selftest(const pr_attempt_mats *am, const pr_commitment *c, uint32_t band) {
    struct pr_thread_ws w;
    memset(&w, 0, sizeof(w));
    w.am = *am; /* borrow the caller's buffers */
    w.c = *c;
    w.acc = malloc(sizeof(int32_t) * 64 * am->n);
    w.tt = malloc(sizeof(uint32_t) * PEARL_ROW_BASES_PER_BAND * PEARL_COL_BASES_PER_BAND *
                  PEARL_TRANSCRIPT_WORDS);
    if (!w.acc || !w.tt) abort();

    run_band(&w, band);
    g_band_selftest_ok = 1;

    for (uint32_t rb = 0; rb < PEARL_ROW_BASES_PER_BAND; rb++) {
        for (uint32_t cb = 0; cb < PEARL_COL_BASES_PER_BAND; cb++) {
            uint32_t t_rows = band * 64 + rb;
            uint32_t t_cols = cb * 2;
            uint32_t words[PEARL_TRANSCRIPT_WORDS];
            uint8_t digest[32];
            pr_tile_scalar(am->ap, am->btp, am->k, t_rows, t_cols, c->a_noise_seed.b, words, digest);
            size_t ti = ((size_t)rb * PEARL_COL_BASES_PER_BAND + cb) * PEARL_TRANSCRIPT_WORDS;
            if (memcmp(w.tt + ti, words, 64) != 0) g_band_selftest_ok = 0;
            uint8_t kd[32];
            pr_blake3_keyed_block64(c->a_noise_seed.b, (const uint8_t *)(w.tt + ti), kd);
            if (memcmp(kd, digest, 32) != 0) g_band_selftest_ok = 0;
        }
    }
    free(w.acc);
    free(w.tt);
}
