/*
 * pearl.c — pearlhash core: blake3 wrappers, V3 commitment chain, rank-128
 * noise, tile transcripts, difficulty bound. Matches the official zk-pow /
 * pearl-blake3 crates bit-for-bit (validated by tests/selftest).
 */
#include "pearl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blake3/blake3.h"
#include "blake3/blake3_impl.h"

/* ------------------------------------------------------------------ hex -- */

void pr_hex_encode(char *out, const uint8_t *in, size_t len) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * len] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pr_hex_decode(uint8_t *out, size_t out_cap, const char *hex, size_t *len_out) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > out_cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    if (len_out) *len_out = n / 2;
    return 0;
}

/* -------------------------------------------------------------- blake3 -- */

static void words_from_key(const uint8_t key[32], uint32_t w[8]) {
    memcpy(w, key, 32); /* little-endian host */
}

void pr_blake3_keyed_block64(const uint8_t key[32], const uint8_t msg[64], uint8_t out32[32]) {
    uint32_t cv[8];
    words_from_key(key, cv);
    blake3_compress_in_place_portable(cv, msg, 64, 0,
                                      KEYED_HASH | CHUNK_START | CHUNK_END | ROOT);
    memcpy(out32, cv, 32);
}

void pr_blake3_block64(const uint8_t msg[64], uint8_t out32[32]) {
    uint32_t cv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                      0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
    blake3_compress_in_place_portable(cv, msg, 64, 0,
                                      CHUNK_START | CHUNK_END | ROOT);
    memcpy(out32, cv, 32);
}

void pr_blake3(const uint8_t *data, size_t len, const uint8_t *key, uint8_t out32[32]) {
    blake3_hasher h;
    if (key) blake3_hasher_init_keyed(&h, key);
    else blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out32, 32);
}

/* pearl_blake3 primitives: chunk CV (non-root), parent CV, root CV. */

void pr_chunk_cv(const uint8_t key[32], const uint8_t chunk[1024], uint64_t chunk_index, uint8_t cv[32]) {
    uint32_t state[8];
    words_from_key(key, state);
    uint8_t flags = KEYED_HASH;
    for (unsigned blk = 0; blk < 16; blk++) {
        uint8_t f = flags;
        if (blk == 0) f |= CHUNK_START;
        if (blk == 15) f |= CHUNK_END;
        blake3_compress_in_place(state, chunk + 64 * blk, 64, chunk_index, f);
    }
    memcpy(cv, state, 32);
}

void pr_parent_cv(const uint8_t key[32], const uint8_t left[32], const uint8_t right[32], uint8_t cv[32]) {
    uint32_t state[8];
    uint8_t block[64];
    words_from_key(key, state);
    memcpy(block, left, 32);
    memcpy(block + 32, right, 32);
    blake3_compress_in_place_portable(state, block, 64, 0, KEYED_HASH | PARENT);
    memcpy(cv, state, 32);
}

void pr_root_cv(const uint8_t key[32], const uint8_t left[32], const uint8_t right[32], uint8_t out[32]) {
    uint32_t state[8];
    uint8_t block[64], xof[64];
    words_from_key(key, state);
    memcpy(block, left, 32);
    memcpy(block + 32, right, 32);
    blake3_compress_xof_portable(state, block, 64, 0,
                                 KEYED_HASH | PARENT | ROOT, xof);
    memcpy(out, xof, 32);
}

/*
 * Root of the pearl_blake3 tree: bottom-up pairing of chunk CVs carrying the
 * odd node up (verified equal to the keyed blake3 of the whole buffer, see
 * tests/selftest). `fast` uses the official SIMD hasher, which builds the same
 * tree; the manual path exists to prove that equality in the selftest.
 */
void pr_matrix_root(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t root[32]) {
    if (len == 0) { memset(root, 0, 32); return; }
    if (len <= 1024) {
        /* single chunk: root is the ROOT-flagged chunk hash */
        uint8_t cv[32];
        uint32_t state[8];
        words_from_key(key, state);
        size_t rem = len;
        const uint8_t *p = data;
        uint8_t flags = KEYED_HASH | CHUNK_START;
        for (unsigned blk = 0; blk < 16 && rem > 0; blk++) {
            uint8_t blockbuf[64];
            size_t take = rem < 64 ? rem : 64;
            memcpy(blockbuf, p, take);
            memset(blockbuf + take, 0, 64 - take);
            uint8_t f = flags;
            if (rem <= 64) f |= CHUNK_END;
            blake3_compress_in_place_portable(state, blockbuf, (uint8_t)take, 0, f);
            p += take; rem -= take;
            flags = KEYED_HASH;
        }
        memcpy(root, state, 32);
        (void)cv;
        return;
    }
    size_t nchunks = (len + 1023) / 1024;
    /* layer-by-layer pairing */
    uint8_t *buf = malloc(nchunks * 32);
    if (!buf) abort();
    for (size_t i = 0; i < nchunks; i++) {
        uint8_t chunk[1024];
        size_t take = len - i * 1024 < 1024 ? len - i * 1024 : 1024;
        memcpy(chunk, data + i * 1024, take);
        memset(chunk + take, 0, 1024 - take);
        pr_chunk_cv(key, chunk, i, buf + i * 32);
    }
    size_t count = nchunks;
    while (count > 2) {
        size_t outn = (count + 1) / 2;
        for (size_t i = 0; i < outn; i++) {
            if (2 * i + 1 < count) {
                uint8_t tmp[32];
                memcpy(tmp, buf + (2 * i) * 32, 32);
                pr_parent_cv(key, tmp, buf + (2 * i + 1) * 32, buf + i * 32);
            } else {
                memmove(buf + i * 32, buf + (2 * i) * 32, 32);
            }
        }
        count = outn;
    }
    if (count == 2) {
        pr_root_cv(key, buf, buf + 32, root);
    } else {
        memcpy(root, buf, 32); /* unreachable for len > 1024 */
    }
    free(buf);
}

/* ---------------------------------------------------------- commitment -- */

void pr_config52(uint32_t k, uint8_t out[PEARL_CONFIG52_LEN]) {
    memset(out, 0, PEARL_CONFIG52_LEN);
    uint16_t rank = (uint16_t)PEARL_RANK, mma = (uint16_t)PEARL_MMA_INT7;
    memcpy(out, &k, 4);                /* common_dim  u32 LE */
    memcpy(out + 4, &rank, 2);         /* rank        u16 LE */
    memcpy(out + 6, &mma, 2);          /* mma_type    u16 LE */
    { uint8_t rows[6] = PEARL_CONFIG_ROWS_BYTES; memcpy(out + 8, rows, 6); }
    { uint8_t cols[6] = PEARL_CONFIG_COLS_BYTES; memcpy(out + 14, cols, 6); }
    /* bytes 20..52: MoE trailer, all zero (dense job) */
}

void pr_commit(const uint8_t header76[PEARL_HEADER76_LEN], uint32_t k,
               const int8_t *a, const int8_t *bt, uint32_t m, uint32_t n,
               pr_commitment *out) {
    uint8_t config[PEARL_CONFIG52_LEN];
    pr_config52(k, config);

    uint8_t msg[128];
    memcpy(msg, header76, 76);
    memcpy(msg + 76, config, 52);
    pr_blake3(msg, 128, NULL, out->job_key.b);

    /* roots over the two's-complement bytes, zero-padded to 1024 (the pad
     * region must be explicit zeros: it is hashed and consensus-visible) */
    size_t alen = (size_t)m * k, blen = (size_t)n * k;
    size_t apad = ((alen + 1023) / 1024) * 1024, bpad = ((blen + 1023) / 1024) * 1024;
    uint8_t *buf = malloc(apad > bpad ? apad : bpad);
    if (!buf) abort();
    memcpy(buf, a, alen);
    memset(buf + alen, 0, apad - alen);
    pr_blake3(buf, apad, out->job_key.b, out->root_a.b);
    memcpy(buf, bt, blen);
    memset(buf + blen, 0, bpad - blen);
    pr_blake3(buf, bpad, out->job_key.b, out->root_b.b);
    free(buf);

    pr_bind_seeds(out->root_a.b, out->root_b.b, m, n, out);
}

void pr_bind_seeds(const uint8_t root_a[32], const uint8_t root_b[32],
                   uint32_t m, uint32_t n, pr_commitment *out) {
    static const uint8_t SALT_A[32] = {
        0x82, 0x49, 0x40, 0x6c, 0xa0, 0xed, 0x15, 0x16, 0x96, 0x16, 0xf6, 0x92, 0xfc, 0xf0, 0x76, 0xf8,
        0x92, 0xdb, 0xdb, 0x2a, 0x70, 0x23, 0xb8, 0x52, 0xf0, 0xd4, 0x77, 0x19, 0xc3, 0x90, 0x01, 0x7b};
    static const uint8_t SALT_B[32] = {
        0x11, 0x30, 0x06, 0x32, 0xec, 0x63, 0x01, 0xca, 0x2b, 0xe2, 0xaf, 0x71, 0x8b, 0x3f, 0x4d, 0x4f,
        0x1a, 0xe9, 0xc6, 0x39, 0x88, 0xe8, 0xcc, 0x04, 0x48, 0x44, 0x30, 0x1d, 0x71, 0xb8, 0x9a, 0xa9};
    uint8_t bind_msg[64];
    memset(bind_msg, 0, 64);
    memcpy(bind_msg, root_a, 32);
    memcpy(bind_msg + 32, &m, 4);
    pr_blake3(bind_msg, 64, SALT_A, out->bound_a.b);
    memcpy(bind_msg, root_b, 32);
    memcpy(bind_msg + 32, &n, 4);
    pr_blake3(bind_msg, 64, SALT_B, out->bound_b.b);

    uint8_t chain[64];
    memcpy(chain, out->job_key.b, 32);
    memcpy(chain + 32, out->bound_b.b, 32);
    pr_blake3(chain, 64, NULL, out->b_noise_seed.b);
    memcpy(chain, out->b_noise_seed.b, 32);
    memcpy(chain + 32, out->bound_a.b, 32);
    pr_blake3(chain, 64, NULL, out->a_noise_seed.b);
}

/* ---------------------------------------------------------------- noise -- */

static const uint8_t LABEL_A[32] = {'A', '_', 't', 'e', 'n', 's', 'o', 'r', 0};
static const uint8_t LABEL_B[32] = {'B', '_', 't', 'e', 'n', 's', 'o', 'r', 0};

void pr_noise_hash(uint64_t index, const uint8_t label[32], const uint8_t key[32], unsigned slot, uint8_t out32[32]) {
    uint8_t msg[64];
    memset(msg, 0, 64);
    uint32_t idx1 = (uint32_t)(index + 1);
    memcpy(msg + 4 * slot, &idx1, 4);
    memcpy(msg + 32, label, 32);
    pr_blake3_keyed_block64(key, msg, out32);
}

void pr_uniform_row(const uint8_t label[32], const uint8_t key[32], uint64_t row, unsigned r, int8_t *out) {
    for (unsigned c = 0; c < r; c++) {
        uint64_t gidx = row * r + c;
        uint8_t h[32];
        pr_noise_hash(gidx / 32, label, key, 0, h);
        out[c] = (int8_t)((h[gidx % 32] & 63) - 32);
    }
}

void pr_perm_pair(const uint8_t label[32], const uint8_t key[32], uint64_t l, unsigned r, uint32_t pair[2]) {
    uint8_t h[32];
    pr_noise_hash(l / 8, label, key, 1, h);
    unsigned w = (unsigned)(l % 8);
    uint32_t x;
    memcpy(&x, h + 4 * w, 4);
    uint32_t p = x & (uint32_t)(r - 1);
    uint32_t q = p ^ (1u + (uint32_t)(((uint64_t)(r - 1) * (uint64_t)x) >> 32));
    pair[0] = p;
    pair[1] = q;
}

/* SplitMix64 (counter based) int7 fill, same as the reference generator. */
static uint64_t sm64_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void fill_int7(uint64_t seed, uint64_t domain, size_t len, int8_t *out) {
    uint64_t state = seed ^ domain;
    size_t i = 0;
    while (i < len) {
        uint64_t w = sm64_mix(state += 0x9e3779b97f4a7c15ULL);
        const uint8_t *bytes = (const uint8_t *)&w;
        for (int b = 0; b < 8 && i < len; b++, i++) out[i] = (int8_t)((bytes[b] & 0x7f) - 64);
    }
}

void pr_fill_base(pr_attempt_mats *am, uint64_t seed) {
    fill_int7(seed, PEARL_DOMAIN_A, (size_t)am->m * am->k, am->a);
    fill_int7(seed, PEARL_DOMAIN_BT, (size_t)am->n * am->k, am->bt);
}

void pr_noise_operands(pr_attempt_mats *am, const pr_commitment *c) {
    const unsigned r = PEARL_RANK;
    const uint32_t m = am->m, n = am->n, k = am->k;

    /* permutation pairs (once per attempt) */
    uint32_t(*pairs_a)[2] = malloc(sizeof(uint32_t[2]) * k);
    uint32_t(*pairs_b)[2] = malloc(sizeof(uint32_t[2]) * k);
    if (!pairs_a || !pairs_b) abort();
    for (uint32_t l = 0; l < k; l++) {
        pr_perm_pair(LABEL_A, c->a_noise_seed.b, l, r, pairs_a[l]);
        pr_perm_pair(LABEL_B, c->b_noise_seed.b, l, r, pairs_b[l]);
    }

    /* E and noised operands, row by row (uniform factor row + sparse product) */
    int8_t *fac = malloc(r); /* one uniform factor row scratch */
    if (!fac) abort();
    for (uint32_t i = 0; i < m; i++) {
        pr_uniform_row(LABEL_A, c->a_noise_seed.b, i, r, fac);
        const int8_t *arow = am->a + (size_t)i * k;
        int8_t *erow = am->ap + (size_t)i * k;
        for (uint32_t l = 0; l < k; l++) {
            int32_t e = (int32_t)fac[pairs_a[l][0]] - (int32_t)fac[pairs_a[l][1]];
            erow[l] = (int8_t)((int32_t)arow[l] + e);
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        pr_uniform_row(LABEL_B, c->b_noise_seed.b, j, r, fac);
        const int8_t *brow = am->bt + (size_t)j * k;
        int8_t *erow = am->btp + (size_t)j * k;
        for (uint32_t l = 0; l < k; l++) {
            int32_t e = (int32_t)fac[pairs_b[l][0]] - (int32_t)fac[pairs_b[l][1]];
            erow[l] = (int8_t)((int32_t)brow[l] + e);
        }
    }
    free(fac);
    free(pairs_a);
    free(pairs_b);

    /* widen to s16 for the madd-based dot kernels */
    size_t alen = (size_t)m * k, blen = (size_t)n * k;
    for (size_t i = 0; i < alen; i++) am->a16[i] = am->ap[i];
    for (size_t i = 0; i < blen; i++) am->bt16[i] = am->btp[i];
}

/* ------------------------------------------------------ tiles (scalar) -- */

static const uint8_t ROW_OFF[PEARL_TILE_ROWS] = {0, 8, 16, 24, 32, 40, 48, 56};
static const uint8_t COL_OFF[PEARL_TILE_COLS] = {0, 1, 8, 9, 16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57};

bool pr_tile_base_valid(uint32_t t_rows, uint32_t t_cols, uint32_t m, uint32_t n) {
    if (t_rows >= m || t_cols >= n) return false;
    if (t_rows % 64 >= PEARL_TILE_ROWS) return false;
    if (t_cols % 64 >= PEARL_TILE_COLS || (t_cols % 2) != 0) return false;
    if (t_rows + ROW_OFF[PEARL_TILE_ROWS - 1] >= m) return false;
    if (t_cols + COL_OFF[PEARL_TILE_COLS - 1] >= n) return false;
    return true;
}

void pr_tile_scalar(const int8_t *ap, const int8_t *btp, uint32_t k, uint32_t t_rows, uint32_t t_cols,
                    const uint8_t a_noise_seed[32], uint32_t transcript[16], uint8_t digest[32]) {
    int32_t acc[PEARL_TILE_ROWS][PEARL_TILE_COLS];
    memset(acc, 0, sizeof(acc));
    memset(transcript, 0, PEARL_TRANSCRIPT_WORDS * 4);
    uint32_t slices = k / PEARL_RANK;
    for (uint32_t s = 0; s < slices; s++) {
        uint32_t lo = s * PEARL_RANK;
        for (unsigned u = 0; u < PEARL_TILE_ROWS; u++) {
            const int8_t *ar = ap + (size_t)(t_rows + ROW_OFF[u]) * k + lo;
            for (unsigned v = 0; v < PEARL_TILE_COLS; v++) {
                const int8_t *br = btp + (size_t)(t_cols + COL_OFF[v]) * k + lo;
                int32_t dot = 0;
                for (uint32_t l = 0; l < PEARL_RANK; l++) dot += (int32_t)ar[l] * (int32_t)br[l];
                acc[u][v] += dot;
            }
        }
        uint32_t fold = 0;
        for (unsigned u = 0; u < PEARL_TILE_ROWS; u++)
            for (unsigned v = 0; v < PEARL_TILE_COLS; v++) fold ^= (uint32_t)acc[u][v];
        transcript[s % PEARL_TRANSCRIPT_WORDS] =
            pr_rotl32(transcript[s % PEARL_TRANSCRIPT_WORDS], PEARL_TRANSCRIPT_ROTL) ^ fold;
    }
    uint8_t msg[64];
    memcpy(msg, transcript, 64);
    pr_blake3_keyed_block64(a_noise_seed, msg, digest);
}

/* ---------------------------------------------------------- difficulty -- */

int pr_u256_cmp(const pr_u256 *a, const pr_u256 *b) {
    for (int i = 3; i >= 0; i--) {
        if (a->w[i] < b->w[i]) return -1;
        if (a->w[i] > b->w[i]) return 1;
    }
    return 0;
}

uint32_t pr_compact_from_target(const pr_u256 *t) {
    if ((t->w[0] | t->w[1] | t->w[2] | t->w[3]) == 0) return 0;
    int bits = 256;
    for (int i = 3; i >= 0; i--) {
        if (t->w[i]) {
            uint64_t v = t->w[i];
            int b = 0;
            while (v) { b++; v >>= 1; }
            bits = i * 64 + b;
            break;
        }
    }
    uint32_t size = (uint32_t)((bits + 7) / 8);
    uint32_t mantissa;
    if (size <= 3) {
        mantissa = (uint32_t)(t->w[0] << (8 * (3 - size)));
    } else {
        uint32_t drop = size - 3; /* bytes dropped from the low end */
        uint32_t wi = drop / 8, within = (drop % 8) * 8;
        uint64_t lo = t->w[wi] >> within;
        uint64_t hi = (within && wi + 1 < 4) ? (t->w[wi + 1] << (64 - within)) : 0;
        mantissa = (uint32_t)(lo | hi);
    }
    if (mantissa & 0x00800000u) { mantissa >>= 8; size += 1; }
    return (size << 24) | (mantissa & 0x00ffffffu);
}

void pr_target_from_compact(uint32_t nbits, pr_u256 *out) {
    memset(out, 0, sizeof(*out));
    uint32_t exponent = nbits >> 24;
    uint32_t mantissa = nbits & 0x00ffffffu;
    if (mantissa == 0 || exponent == 0 || (mantissa & 0x00800000u)) return;
    if (exponent <= 3) {
        uint64_t v = mantissa >> (8 * (3 - exponent));
        out->w[0] = v;
    } else {
        uint32_t shift = 8 * (exponent - 3);
        uint32_t wi = shift / 64, within = shift % 64;
        if (wi < 4) {
            out->w[wi] |= (uint64_t)mantissa << within;
            if (within && wi + 1 < 4) out->w[wi + 1] = (uint64_t)mantissa >> (64 - within);
        }
    }
}

static void u256_mul_u32(pr_u256 *x, uint64_t f) { /* f <= 2^32, in place, saturating */
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 p = (unsigned __int128)x->w[i] * f + carry;
        x->w[i] = (uint64_t)p;
        carry = (uint64_t)(p >> 64);
    }
    if (carry) { /* saturate to max */
        for (int i = 0; i < 4; i++) x->w[i] = ~0ULL;
    }
}

bool pr_share_bound(const uint8_t target_be[32], uint32_t k, pr_u256 *bound) {
    /* target (big-endian byte string) -> u256 LE words */
    pr_u256 t;
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | target_be[i * 8 + j];
        t.w[3 - i] = v;
    }
    /* pools verify with nbits_override = compact(target): effective target = expand(compact) */
    uint32_t nbits = pr_compact_from_target(&t);
    pr_u256 eff;
    pr_target_from_compact(nbits, &eff);
    if (pr_u256_cmp(&eff, &t) > 0) eff = t;

    uint64_t factor = (uint64_t)PEARL_TILE_SIZE * (k - k % PEARL_RANK);
    if (factor == 0) return false; /* k < rank: no committed slices, no valid bound */
    /* overflow check: eff > MAX / factor -> refuse */
    pr_u256 max; for (int i = 0; i < 4; i++) max.w[i] = ~0ULL;
    pr_u256 q; memset(&q, 0, sizeof(q)); /* divide max by factor (small) */
    uint64_t rem = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned __int128 cur = ((unsigned __int128)rem << 64) | max.w[i];
        q.w[i] = (uint64_t)(cur / factor);
        rem = (uint64_t)(cur % factor);
    }
    if (pr_u256_cmp(&eff, &q) > 0) return false;
    *bound = eff;
    u256_mul_u32(bound, factor);
    return true;
}

/* ---------------------------------------------- incremental host tree --- */

enum { HT_MAXL = 48 };

struct pr_htree {
    uint8_t key[32];
    uint64_t nleaves;
    int nlayers;
    uint8_t *layers[HT_MAXL];
    uint64_t lens[HT_MAXL];
};

pr_htree *pr_htree_new(const uint8_t key[32], const uint8_t *data, size_t len) {
    if (len == 0) return NULL;
    pr_htree *t = calloc(1, sizeof(*t));
    if (!t) abort();
    memcpy(t->key, key, 32);
    t->nleaves = (len + 1023) / 1024;
    t->layers[0] = malloc(t->nleaves * 32);
    if (!t->layers[0]) abort();
    t->lens[0] = t->nleaves;
    t->nlayers = 1;
    for (uint64_t i = 0; i < t->nleaves; i++) {
        uint8_t chunk[1024];
        size_t take = len - i * 1024 < 1024 ? len - i * 1024 : 1024;
        memcpy(chunk, data + i * 1024, take);
        memset(chunk + take, 0, 1024 - take);
        pr_chunk_cv(key, chunk, i, t->layers[0] + i * 32);
    }
    uint64_t count = t->nleaves;
    while (count > 2) {
        uint64_t outn = (count + 1) / 2;
        uint8_t *next = malloc((outn + 1) * 32);
        if (!next) abort();
        for (uint64_t i = 0; i < outn; i++) {
            if (2 * i + 1 < count)
                pr_parent_cv(key, t->layers[t->nlayers - 1] + 2 * i * 32,
                             t->layers[t->nlayers - 1] + (2 * i + 1) * 32, next + i * 32);
            else
                memcpy(next + i * 32, t->layers[t->nlayers - 1] + 2 * i * 32, 32);
        }
        t->layers[t->nlayers] = next;
        t->lens[t->nlayers] = outn;
        t->nlayers++;
        count = outn;
    }
    return t;
}

void pr_htree_patch_chunk(pr_htree *t, const uint8_t key[32], uint64_t chunk,
                          const uint8_t data1024[1024]) {
    if (!t || chunk >= t->nleaves) return;
    pr_chunk_cv(key, data1024, chunk, t->layers[0] + chunk * 32);
    uint64_t idx = chunk;
    for (int L = 0; L + 1 < t->nlayers; L++) {
        uint64_t p = idx / 2;
        if (2 * p + 1 < t->lens[L])
            pr_parent_cv(key, t->layers[L] + 2 * p * 32, t->layers[L] + (2 * p + 1) * 32,
                         t->layers[L + 1] + p * 32);
        else
            memcpy(t->layers[L + 1] + p * 32, t->layers[L] + 2 * p * 32, 32);
        idx = p;
    }
}

void pr_htree_root(pr_htree *t, uint8_t out[32]) {
    if (!t || t->nlayers == 0) { memset(out, 0, 32); return; }
    if (t->nleaves == 1) { memcpy(out, t->layers[0], 32); return; }
    const uint8_t *top = t->layers[t->nlayers - 1];
    pr_root_cv(t->key, top, top + 32, out);
}

void pr_htree_free(pr_htree *t) {
    if (!t) return;
    for (int i = 0; i < t->nlayers; i++) free(t->layers[i]);
    free(t);
}
