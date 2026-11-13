/*
 * pearl.h — A CPU miner ("C pickaxe") for Pearl (PRL), the Proof-of-Useful-Work L1.
 *
 * Implements the consensus "pearlhash" (certificate V3, salted): a noisy int8
 * matrix multiplication whose per-tile transcripts are BLAKE3-hashed into
 * lottery tickets. Spec extracted from the official zk-pow / pearl-blake3
 * crates (pearl-research-labs/pearl @ ISC) and the spm-cpuref reference.
 *
 * Layout of one mining attempt (all miner-chosen, pool fixes only the header
 * and the share target):
 *   1. pick a seed -> int7 matrices A (m×k), Bᵀ (n×k), entries in [-64, 63]
 *   2. commit: job_key = blake3(header76 || config52)
 *              root_a/b = keyed-blake3(job_key, pad1024(matrix bytes))
 *              bound_a/b = salted roots (SEED_SALT_A/B, dimension committed)
 *              b_noise_seed = blake3(job_key || bound_b)
 *              a_noise_seed = blake3(b_noise_seed || bound_a)
 *   3. noise: E_A = A_L·A_R, E_B = B_L·B_R (rank r = 128, BLAKE3 PRNG)
 *      A' = A + E_A, B'ᵀ = Bᵀ + E_Bᵀ (exact s8)
 *   4. per 64-row band of the output: sliced GEMM C' += A'·B'ᵀ in i32;
 *      after every r-wide slice each 8×16 hash tile XOR-folds its cumulative
 *      128 accumulators into transcript word s mod 16 (rotl 13 before xor);
 *      after the last slice digest = blake3_keyed(a_noise_seed, transcript)
 *      and a tile is a hit when the little-endian u256 digest <= bound.
 *   5. build the PlainProof (multileaf Merkle openings of the tile's rows)
 *      and submit bincode(fixint LE) -> base64 as `plain_proof`.
 */
#ifndef PEARL_H
#define PEARL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- consensus constants ------------------------------------------------ */

#define PEARL_RANK 128u /* noise rank since the RankPenalty fork (r=128, no penalty) */
#define PEARL_MMA_INT7 0u
#define PEARL_CONFIG52_LEN 52
#define PEARL_HEADER76_LEN 76
#define PEARL_TILE_ROWS 8u  /* rows   {0,8,...,56}  (period 64) */
#define PEARL_TILE_COLS 16u /* cols   {0,1,8,...,57} (period 64) */
#define PEARL_TILE_SIZE (PEARL_TILE_ROWS * PEARL_TILE_COLS) /* 128 outputs */
#define PEARL_TRANSCRIPT_WORDS 16u
#define PEARL_TRANSCRIPT_ROTL 13u

/* config52 byte layout (MiningConfiguration::to_bytes) */
#define PEARL_CONFIG_ROWS_BYTES {7, 7, 0, 0, 0, 0}           /* rows pattern   [(8,8),(64,1),(64,1)]  */
#define PEARL_CONFIG_COLS_BYTES {0, 1, 3, 7, 0, 0}           /* cols pattern   [(1,2),(8,8),(64,1)]   */

/* domain separators for the deterministic int7 matrix generator (spm-b compatible) */
#define PEARL_DOMAIN_A 0x73706d2d612d3031ULL  /* "spm-a-01" */
#define PEARL_DOMAIN_BT 0x73706d2d622d3031ULL /* "spm-b-01" */

typedef struct { uint8_t b[32]; } pr_h256;

/* little-endian u256 (word 0 = least significant) */
typedef struct { uint64_t w[4]; } pr_u256;

/* ---- little helpers ----------------------------------------------------- */

void pr_hex_encode(char *out, const uint8_t *in, size_t len);
int pr_hex_decode(uint8_t *out, size_t out_cap, const char *hex, size_t *len_out);

static inline uint32_t pr_rotl32(uint32_t x, unsigned r) {
    return (uint32_t)((x << r) | (x >> ((32 - r) & 31)));
}

/* ---- blake3 wrappers ---------------------------------------------------- */

/* keyed blake3 of a single 64-byte block (noise hashes, jackpot digests) */
void pr_blake3_keyed_block64(const uint8_t key[32], const uint8_t msg[64], uint8_t out32[32]);
/* unkeyed blake3 of a single 64-byte block */
void pr_blake3_block64(const uint8_t msg[64], uint8_t out32[32]);
/* plain/keyed blake3 of an arbitrary buffer (uses the vendored SIMD lib) */
void pr_blake3(const uint8_t *data, size_t len, const uint8_t *key /* NULL = unkeyed */, uint8_t out32[32]);

/* pearl_blake3 merkle primitives (keyed) */
void pr_chunk_cv(const uint8_t key[32], const uint8_t chunk[1024], uint64_t chunk_index, uint8_t cv[32]);
void pr_parent_cv(const uint8_t key[32], const uint8_t left[32], const uint8_t right[32], uint8_t cv[32]);
void pr_root_cv(const uint8_t key[32], const uint8_t left[32], const uint8_t right[32], uint8_t out[32]);
/* full keyed blake3 tree root of padded data (== keyed blake3 of the bytes) */
void pr_matrix_root(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t root[32]);

/* ---- commitment --------------------------------------------------------- */

/* host-side incremental pearl-blake3 Merkle tree over a matrix buffer.
 * patch_chunk recomputes one 1024B chunk CV plus its path (~10 hashes), so a
 * nonce change costs microseconds instead of a full re-hash. */
typedef struct pr_htree pr_htree;
pr_htree *pr_htree_new(const uint8_t key[32], const uint8_t *data, size_t len);
void pr_htree_patch_chunk(pr_htree *t, const uint8_t key[32], uint64_t chunk,
                          const uint8_t data1024[1024]);
void pr_htree_root(pr_htree *t, uint8_t out[32]);
void pr_htree_free(pr_htree *t);

/* config52 for our fixed mining configuration (rank 128, 8x16 tiles, int7) */
void pr_config52(uint32_t k, uint8_t out[PEARL_CONFIG52_LEN]);

typedef struct {
    pr_h256 job_key, root_a, root_b, bound_a, bound_b, b_noise_seed, a_noise_seed;
} pr_commitment;

/* V3 binding + seed chain from (possibly patched) roots: fills bound_a/bound_b,
 * b_noise_seed, a_noise_seed of `out` (job_key/root_a/root_b must be set). */
void pr_bind_seeds(const uint8_t root_a[32], const uint8_t root_b[32],
                   uint32_t m, uint32_t n, pr_commitment *out);

/* Full V3 commitment for matrices a (m×k) / bt (n×k), row-major int8. */
void pr_commit(const uint8_t header76[PEARL_HEADER76_LEN], uint32_t k,
               const int8_t *a, const int8_t *bt, uint32_t m, uint32_t n,
               pr_commitment *out);

/* ---- noise (official zk-pow generators) --------------------------------- */

/* hash used by the noise PRNG: msg64 zero except (index+1) i32 LE at 4*slot and label at 32..64 */
void pr_noise_hash(uint64_t index, const uint8_t label[32], const uint8_t key[32], unsigned slot, uint8_t out32[32]);

/* uniform factor row `row` (r entries in [-32,31]) */
void pr_uniform_row(const uint8_t label[32], const uint8_t key[32], uint64_t row, unsigned r, int8_t *out);
/* permutation pair l: p = x & (r-1), q = p ^ (1 + mulhi32(r-1, x)) */
void pr_perm_pair(const uint8_t label[32], const uint8_t key[32], uint64_t l, unsigned r, uint32_t pair[2]);

/* per-attempt matrix workspace */
typedef struct {
    uint32_t m, n, k;
    int8_t *a, *bt;    /* base matrices (the bytes the Merkle roots commit) */
    int8_t *ap, *btp;  /* A' = A + E_A, B'ᵀ = Bᵀ + E_Bᵀ */
    int16_t *a16, *bt16; /* widened operands for the SIMD dot kernels */
} pr_attempt_mats;

/* stage 1: deterministic int7 fill of the base matrices */
void pr_fill_base(pr_attempt_mats *am, uint64_t seed);
/* stage 3: rank-128 noise, noised operands (exact s8), widened copies */
void pr_noise_operands(pr_attempt_mats *am, const pr_commitment *c);

/* hash-tile offset patterns (period 64 in both dimensions) */
static inline uint32_t pr_row_off(unsigned u) { return (uint32_t)u * 8u; }
static inline uint32_t pr_col_off(unsigned v) { return (v / 2u) * 8u + (v % 2u); }
/* valid tile bases per 64-row / 64-col band: rows {0..7}, cols {0,2,4,6} */
#define PEARL_ROW_BASES_PER_BAND 8u
#define PEARL_COL_BASES_PER_BAND 4u

/* ---- tiles / transcripts ------------------------------------------------ */

bool pr_tile_base_valid(uint32_t t_rows, uint32_t t_cols, uint32_t m, uint32_t n);
/* scalar reference transcript+digest of one tile (used by selftest) */
void pr_tile_scalar(const int8_t *ap, const int8_t *btp, uint32_t k, uint32_t t_rows, uint32_t t_cols,
                    const uint8_t a_noise_seed[32], uint32_t transcript[16], uint8_t digest[32]);

/* ---- difficulty --------------------------------------------------------- */

/* Bitcoin compact nbits <-> u256 target (official semantics) */
uint32_t pr_compact_from_target(const pr_u256 *t);
void pr_target_from_compact(uint32_t nbits, pr_u256 *out);
/* bound = min(expand(compact(target)), target) * tile_size * dot_len; false on overflow */
bool pr_share_bound(const uint8_t target_be[32], uint32_t k, pr_u256 *bound);
int pr_u256_cmp(const pr_u256 *a, const pr_u256 *b);

/* ---- proof building / wire format --------------------------------------- */

typedef struct {
    uint8_t *data;     /* bincode bytes */
    size_t len;
    char *b64;         /* NUL-terminated standard base64 */
    size_t b64_len;
} pr_share;

/*
 * Builds the share for tile (t_rows, t_cols): multileaf Merkle openings of the
 * tile's 8 A rows and 16 Bᵀ rows, serialized as bincode (fixint LE) PlainProof.
 * a16/ap are unused here; a/bt must hold the base matrices.
 */
bool pr_build_share(const pr_attempt_mats *am, const pr_commitment *c,
                    uint32_t t_rows, uint32_t t_cols, pr_share *out);
void pr_share_free(pr_share *s);

/* ---- mining kernel ------------------------------------------------------ */

typedef struct pr_ctx pr_ctx;

pr_ctx *pr_ctx_new(uint32_t m, uint32_t n, uint32_t k, unsigned n_threads);
void pr_ctx_free(pr_ctx *ctx);
uint64_t pr_ctx_macs(const pr_ctx *ctx, unsigned thread_id);
/* select the fastest dot128 kernel (AVX2 madd path when available) */
void pr_dot_dispatch(void);

/* One attempt: generate matrices, commit, scan every tile; returns 1 and fills
 * `share` when a tile digest meets the bound of `header`/`target_be`. */
bool pr_attempt(pr_ctx *ctx, unsigned thread_id, const uint8_t header76[76],
                const uint8_t target_be[32], const char *job_id, uint64_t seed,
                pr_share *share, uint32_t *hit_t_rows, uint32_t *hit_t_cols,
                uint64_t *macs_done);

/* selftest: run one band through the real kernel and compare against the
 * scalar tile implementation; pr_band_selftest_ok() reports the result */
void pr_band_selftest(const pr_attempt_mats *am, const pr_commitment *c, uint32_t band);
int pr_band_selftest_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* PEARL_H */
