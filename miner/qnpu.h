/* B'ᵀ and its noise are built once per job on the host and uploaded in slice-transposed layout [S,128,n]. */
#ifndef PRL_QNPU_H
#define PRL_QNPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pearl.h"

#define PRL_QNPU_MAXHITS 1024

typedef struct pr_qnpu_ctx pr_qnpu_ctx;

/* > 0 when the HTP backend loads and reports the v79 arch; a short human
 * readable reason when 0 */
int pr_qnpu_device_count(char *reason, size_t cap);

pr_qnpu_ctx *pr_qnpu_new(uint32_t m, uint32_t n, uint32_t k, char *err, size_t errcap);
void pr_qnpu_free(pr_qnpu_ctx *ctx);

/* full job (re)setup: fills A_base/Bᵀ, computes the commitment chain, builds
 * B-side noise + B'ᵀ on the host, uploads to the graph's B input */
int pr_qnpu_set_job(pr_qnpu_ctx *ctx, const uint8_t header76[76], char *err, size_t errcap);

/* upload the share bound for the current pool job (32 bytes = U256 little-endian) */
void pr_qnpu_set_bound(pr_qnpu_ctx *ctx, const uint8_t bound_le[32]);

/* coords[i] = t_rows | ((uint64_t)t_cols << 32); digests = 32 bytes per hit. */
int pr_qnpu_attempt(pr_qnpu_ctx *ctx, uint64_t nonce, uint64_t *coords, uint8_t *digests);

/* Pipelined form (miner loop): queues the attempt for `nonce` on the NPU and returns the PREVIOUS queued attempt's hits; while the NPU executes, the caller can build/submit the returned share. */
int pr_qnpu_attempt_pipe(pr_qnpu_ctx *ctx, uint64_t nonce, uint64_t *coords, uint8_t *digests);
/* zero-idle form: the caller also passes the NEXT nonce so the backend can pre-build its noise while the NPU executes the current attempt. */
int pr_qnpu_attempt_pipe2(pr_qnpu_ctx *ctx, uint64_t nonce, uint64_t next_nonce,
                          uint64_t *coords, uint8_t *digests);
/* The miner should stop instead of retrying a known-failed DSP path for every nonce. */
int pr_qnpu_fused_failed(const pr_qnpu_ctx *ctx);
/* Results are read one attempt at a time so each hit retains its own nonce, A matrix, and commitment for PlainProof construction. */
int pr_qnpu_compound_available(pr_qnpu_ctx *ctx);
uint32_t pr_qnpu_compound_attempts(pr_qnpu_ctx *ctx);
int pr_qnpu_compound_pipe(pr_qnpu_ctx *ctx, uint64_t first_nonce);
int pr_qnpu_compound_result(pr_qnpu_ctx *ctx, uint32_t attempt,
                            uint64_t *nonce, uint64_t *coords,
                            uint8_t *digests);
/* Call before a job change so hits are submitted with the old job id, and at loop exit. */
int pr_qnpu_drain(pr_qnpu_ctx *ctx, uint64_t *coords, uint8_t *digests);

/* state needed to build the PlainProof for a hit on the host */
const int8_t *pr_qnpu_host_a(pr_qnpu_ctx *ctx);            /* A_base incl. nonce patch */
const int8_t *pr_qnpu_host_bt(pr_qnpu_ctx *ctx);
const pr_commitment *pr_qnpu_commitment(pr_qnpu_ctx *ctx); /* reflects the last attempt */
uint64_t pr_qnpu_macs(pr_qnpu_ctx *ctx);

/* set the deterministic base-matrix seed before pr_qnpu_set_job (selftest) */
void pr_qnpu_set_base_seed(pr_qnpu_ctx *ctx, uint64_t seed);

/* debug: copy n bytes from the host-side A' slice buffer (selftest) */
void pr_qnpu_debug_read_ap(pr_qnpu_ctx *ctx, void *dst, size_t n);
/* debug: read the raw slice accumulators of one tile after the last attempt
 * out[s][u][v] = i32 acc of tile (t_rows,t_cols), slice s, cell (u,v) */
void pr_qnpu_debug_acc(pr_qnpu_ctx *ctx, uint32_t t_rows, uint32_t t_cols,
                       int32_t out[16][8][16]);

/* profiling: nanoseconds of the last attempt (noise / execute / fold+digest) */
void pr_qnpu_last_timing(pr_qnpu_ctx *ctx, double *noise_ms, double *exec_ms, double *fold_ms);

#ifdef __cplusplus
}
#endif

#endif /* PRL_QNPU_H */
