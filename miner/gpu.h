/* One attempt = patch a 56-bit nonce into A chunk 0 (host), incrementally
 * update the A Merkle root, then the GPU rebuilds the A-side rank-128 noise,
 * A' = A + E_A, runs the sliced int8 GEMM with per-slice transcript folds,
 * hashes each tile's digest and reports tiles meeting the share bound. */
#ifndef PRL_GPU_H
#define PRL_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pearl.h"

#define PRL_GPU_MAXHITS 1024

typedef struct pr_gpu_ctx pr_gpu_ctx;

/* > 0 when a CUDA device is available */
int pr_gpu_device_count(void);

pr_gpu_ctx *pr_gpu_new(uint32_t m, uint32_t n, uint32_t k, char *err, size_t errcap);
void pr_gpu_free(pr_gpu_ctx *ctx);

/* Safe to call again when the pool job (header) changes. */
int pr_gpu_set_job(pr_gpu_ctx *ctx, const uint8_t header76[76], char *err, size_t errcap);

/* upload the share bound for the current pool job (32 bytes = U256 little-endian) */
void pr_gpu_set_bound(pr_gpu_ctx *ctx, const uint8_t bound_le[32]);

/* coords[i] = t_rows | ((uint64_t)t_cols << 32); digests = 64 × 32 bytes. */
int pr_gpu_attempt(pr_gpu_ctx *ctx, uint64_t nonce, uint64_t *coords, uint8_t *digests);

/* state needed to build the PlainProof for a hit on the host */
const int8_t *pr_gpu_host_a(pr_gpu_ctx *ctx);          /* A_base incl. current nonce patch */
const int8_t *pr_gpu_host_bt(pr_gpu_ctx *ctx);
const pr_commitment *pr_gpu_commitment(pr_gpu_ctx *ctx); /* reflects the last attempt */
uint64_t pr_gpu_macs(pr_gpu_ctx *ctx);

/* set the deterministic base-matrix seed before pr_gpu_set_job (selftest) */
void pr_gpu_set_base_seed(pr_gpu_ctx *ctx, uint64_t seed);
/* enable the in-kernel debug dumps (selftest only; costs throughput) */
void pr_gpu_set_debug(pr_gpu_ctx *ctx, int enable);
/* select the tensor-core (mma.sync) kernel; default on */
void pr_gpu_set_mma(pr_gpu_ctx *ctx, int enable);
int pr_gpu_get_mma(pr_gpu_ctx *ctx);

/* debug: copy n bytes from the device A' buffer (selftest) */
void pr_gpu_debug_read_ap(pr_gpu_ctx *ctx, void *dst, size_t n);
/* debug: device BLAKE3 KAT — hash msg64 with key32 on the device into out32 */
void pr_gpu_debug_hash64(const uint8_t key[32], const uint8_t msg[64], uint8_t out32[32]);
/* debug: after an attempt, the 32 transcripts of block (band, cband) */
void pr_gpu_debug_transcripts(pr_gpu_ctx *ctx, uint32_t band, uint32_t cband, uint32_t out[32][16]);
/* debug: after an attempt, the per-slice folds of the 32 tiles of block (band, cband) */
void pr_gpu_debug_folds(pr_gpu_ctx *ctx, uint32_t band, uint32_t cband, uint32_t out[16][32]);
/* debug: device B'ᵀ bytes / raw accumulators of block (0,0) threads 0..63 after slice 0 */
void pr_gpu_debug_read_btp(pr_gpu_ctx *ctx, void *dst, size_t n);
void pr_gpu_debug_acc(pr_gpu_ctx *ctx, uint32_t out[64][16]);
void pr_gpu_debug_smem(pr_gpu_ctx *ctx, uint8_t out[64 * 128 * 2]); /* As|Bs rows after slice-0 load */
void pr_gpu_debug_accmma(pr_gpu_ctx *ctx, uint32_t out[128]); /* mma lane-0 accs after slab 0 */
void pr_gpu_debug_bfr(pr_gpu_ctx *ctx, uint32_t out[16]); /* lane-0 B fragments at slab0 step0 */
void pr_gpu_debug_bs_mma(pr_gpu_ctx *ctx, uint8_t out[4 * 64 * 128]);

#ifdef __cplusplus
}
#endif

#endif /* PRL_GPU_H */
