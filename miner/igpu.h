/* No CUDA/ROCm/HIP dependency: the backend dlopens the system OpenCL runtime (Mesa Rusticl/Clover, AMD PAL or ROCm all work). */
#ifndef PRL_IGPU_H
#define PRL_IGPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "pearl.h"

#define PRL_IGPU_MAXHITS 1024

typedef struct pr_igpu_ctx pr_igpu_ctx;

/* > 0 when an OpenCL GPU device is available; a short human-readable reason
 * (missing runtime, no device, ...) when 0 */
int pr_igpu_device_count(char *reason, size_t cap);

pr_igpu_ctx *pr_igpu_new(uint32_t m, uint32_t n, uint32_t k, char *err, size_t errcap);
void pr_igpu_free(pr_igpu_ctx *ctx);

/* full job (re)setup: fills A_base/Bᵀ, computes the commitment chain, builds
 * B-side noise + B'ᵀ on the device */
int pr_igpu_set_job(pr_igpu_ctx *ctx, const uint8_t header76[76], char *err, size_t errcap);

/* upload the share bound for the current pool job (32 bytes = U256 little-endian) */
void pr_igpu_set_bound(pr_igpu_ctx *ctx, const uint8_t bound_le[32]);

/* coords[i] = t_rows | ((uint64_t)t_cols << 32); digests = 32 bytes per hit. */
int pr_igpu_attempt(pr_igpu_ctx *ctx, uint64_t nonce, uint64_t *coords, uint8_t *digests);

/* state needed to build the PlainProof for a hit on the host */
const int8_t *pr_igpu_host_a(pr_igpu_ctx *ctx);          /* A_base incl. nonce patch */
const int8_t *pr_igpu_host_bt(pr_igpu_ctx *ctx);
const pr_commitment *pr_igpu_commitment(pr_igpu_ctx *ctx); /* reflects the last attempt */
uint64_t pr_igpu_macs(pr_igpu_ctx *ctx);

/* set the deterministic base-matrix seed before pr_igpu_set_job (selftest) */
void pr_igpu_set_base_seed(pr_igpu_ctx *ctx, uint64_t seed);

/* debug: copy n bytes from the device A' buffer (selftest) */
void pr_igpu_debug_read_ap(pr_igpu_ctx *ctx, void *dst, size_t n);
/* debug: device BLAKE3 KAT — hash msg64 with key32 on the device into out32 */
void pr_igpu_debug_hash64(pr_igpu_ctx *ctx, const uint8_t key[32], const uint8_t msg[64],
                          uint8_t out32[32]);

#ifdef __cplusplus
}
#endif

#endif /* PRL_IGPU_H */
