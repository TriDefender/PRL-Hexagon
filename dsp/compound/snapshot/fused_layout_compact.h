/* Integer-only VTCM layout proposal; no allocation, device API, or core change.
 *
 * All offsets depend only on n and the fixed build-time sizes supplied below.
 * They never depend on m, flags, active worker count, or staging policy. Thus
 * an empty-W switch can retain WH, correction, bias, and complement caches.
 *
 * The existing prefix (x/res/cum/wh/dv/scratch/ttmp/pipe_raw/bias) is preserved.
 * Four worker rings, x2, and DMA descriptors precede the optional complement
 * cache. Ordinary bias256 multi-worker calls no longer allocate an unused
 * complemented-W matrix and its constant tiles merely to reach their rings.
 *
 * Suggested sizes at the integration site:
 *   { PRL_PIPE_SLOTS*PRL_PIPE_SLOT_BYTES,
 *     PRL_HMX_BIAS32_CONFIG_BYTES,
 *     PRL_MULTI_RAW_BYTES(1), PRL_DMA_DESCRIPTOR_BYTES }
 * PRL_MULTI_MAX_WORKERS must equal PRL_LAYOUT_COMPACT_MAX_WORKERS (4).
 * The band transport may share the rings when its per-worker span is the same.
 * This helper does not replace fused_run_core's feature/flag validator.
 */
#ifndef PRL_FUSED_LAYOUT_COMPACT_H
#define PRL_FUSED_LAYOUT_COMPACT_H

#include <stdint.h>

enum {
    PRL_LAYOUT_COMPACT_ALIGNMENT = 2048,
    PRL_LAYOUT_COMPACT_BLOCK_BYTES = 131072,
    PRL_LAYOUT_COMPACT_MAX_WORKERS = 4,
    PRL_LAYOUT_COMPACT_CONSTANT_BYTES = 2048
};

typedef struct {
    uint32_t pipe_raw_bytes;
    uint32_t bias32_config_bytes;
    uint32_t worker_raw_bytes;
    uint32_t dma_descriptor_bytes;
} prl_layout_compact_sizes;

typedef struct {
    uint32_t n,wh_bytes,dv_bytes,ttmp_bytes,bias_bytes;
    prl_layout_compact_sizes sizes;
    uint32_t x,res,cum,wh,dv,scratch,ttmp,pipe_raw,bias_configs;
    uint32_t multi_raw,dma_x2,dma_descriptors;
    uint32_t complement,constant_activation,constant_weight,end;
} prl_layout_compact;

static inline uint64_t prl_layout_compact_align(uint64_t value) {
    return (value+UINT64_C(2047))&~UINT64_C(2047);
}

/* Return 0 on success, -1 on invalid dimensions/sizes or uint32 overflow.
 * A failed initialization leaves *out untouched. Supplies fixed storage for
 * all four worker rings even when a later call uses fewer workers. */
static inline int prl_layout_compact_init(prl_layout_compact *out,uint32_t n,
                                         const prl_layout_compact_sizes *sizes) {
    if (!out||!sizes||n<64||n>2048||n%64||
        sizes->pipe_raw_bytes<8192||sizes->pipe_raw_bytes%2048||
        sizes->bias32_config_bytes<1024||sizes->bias32_config_bytes%256||
        sizes->worker_raw_bytes<8192||sizes->worker_raw_bytes%2048||
        sizes->dma_descriptor_bytes<256||sizes->dma_descriptor_bytes%128) return -1;
    prl_layout_compact r={0};
    r.n=n;r.sizes=*sizes;r.wh_bytes=16u*n*128u;r.dv_bytes=16u*n*4u;
    r.ttmp_bytes=n*32u;
    const uint64_t bias_bytes=16u*(uint64_t)(n/32)*sizes->bias32_config_bytes;
    const uint64_t wh=prl_layout_compact_align(147456u+64u*(uint64_t)n*4u);
    const uint64_t dv=prl_layout_compact_align(wh+r.wh_bytes);
    const uint64_t scratch=prl_layout_compact_align(dv+r.dv_bytes);
    const uint64_t ttmp=scratch+2048u;
    const uint64_t pipe_raw=prl_layout_compact_align(ttmp+r.ttmp_bytes);
    const uint64_t bias=prl_layout_compact_align(pipe_raw+sizes->pipe_raw_bytes);
    const uint64_t multi=prl_layout_compact_align(bias+bias_bytes);
    const uint64_t x2=prl_layout_compact_align(multi+
        (uint64_t)PRL_LAYOUT_COMPACT_MAX_WORKERS*sizes->worker_raw_bytes);
    const uint64_t descriptors=x2+PRL_LAYOUT_COMPACT_BLOCK_BYTES;
    const uint64_t complement=prl_layout_compact_align(descriptors+sizes->dma_descriptor_bytes);
    const uint64_t constant_activation=prl_layout_compact_align(complement+r.wh_bytes);
    const uint64_t constant_weight=constant_activation+PRL_LAYOUT_COMPACT_CONSTANT_BYTES;
    const uint64_t end=constant_weight+PRL_LAYOUT_COMPACT_CONSTANT_BYTES;
    if (end>UINT32_MAX||bias_bytes>UINT32_MAX) return -1;
    r.x=0;r.res=PRL_LAYOUT_COMPACT_BLOCK_BYTES;r.cum=r.res+16384u;
    r.wh=(uint32_t)wh;r.dv=(uint32_t)dv;r.scratch=(uint32_t)scratch;
    r.ttmp=(uint32_t)ttmp;r.pipe_raw=(uint32_t)pipe_raw;r.bias_configs=(uint32_t)bias;
    r.bias_bytes=(uint32_t)bias_bytes;r.multi_raw=(uint32_t)multi;
    r.dma_x2=(uint32_t)x2;r.dma_descriptors=(uint32_t)descriptors;
    r.complement=(uint32_t)complement;r.constant_activation=(uint32_t)constant_activation;
    r.constant_weight=(uint32_t)constant_weight;r.end=(uint32_t)end;
    *out=r;
    return 0;
}

/* actual_workers is the runtime count, already clamped to n/64 and at most 4.
 * use_stage describes the active software stager; a compile-time enabled but
 * unused stager passes 0. No flag inference changes the fixed offsets.
 *
 * Take MAX of all live ends, rather than a priority-ordered ternary. In
 * particular flags128 must retain the complement+constant tail even when
 * workers, DMA, or software staging also need earlier storage. It therefore
 * includes the address span of the reserved raw/DMA regions before that tail,
 * without enabling or executing DMA. Empty gaps are not separate live buffers.
 */
static inline int prl_layout_compact_need(const prl_layout_compact *p,uint32_t flags,
        uint32_t actual_workers,int use_stage,uint32_t *out_need) {
    if (!p||!out_need||p->n<64||p->n>2048||p->n%64||flags>65535u||
        actual_workers>PRL_LAYOUT_COMPACT_MAX_WORKERS||actual_workers>p->n/64||
        (use_stage!=0&&use_stage!=1)) return -1;
    uint64_t need=(uint64_t)p->ttmp+p->ttmp_bytes;
    uint64_t end;
    if (flags&64u) {
        end=(uint64_t)p->pipe_raw+p->sizes.pipe_raw_bytes;
        if (end>need) need=end;
    }
    if (flags&256u) {
        end=(uint64_t)p->bias_configs+p->bias_bytes;
        if (end>need) need=end;
    }
    if (actual_workers) {
        end=(uint64_t)p->multi_raw+(uint64_t)actual_workers*p->sizes.worker_raw_bytes;
        if (end>need) need=end;
    }
    if (use_stage) {
        end=(uint64_t)p->dma_x2+PRL_LAYOUT_COMPACT_BLOCK_BYTES;
        if (end>need) need=end;
    }
    if (flags&4096u) {
        end=(uint64_t)p->dma_descriptors+p->sizes.dma_descriptor_bytes;
        if (end>need) need=end;
    }
    if (flags&128u) {
        end=(uint64_t)p->constant_weight+PRL_LAYOUT_COMPACT_CONSTANT_BYTES;
        if (end>need) need=end;
    }
    if (need>UINT32_MAX) return -1;
    *out_need=(uint32_t)need;
    return 0;
}

/* Same 256-byte cfg placement as fused_run_core. Return 0 iff need fits before
 * cfg; return -1 for insufficient capacity or invalid cfg arguments. On a
 * capacity failure (valid sizes), *out_cfg still reports the reserved cfg base.
 */
static inline int prl_layout_compact_fit(uint32_t need,uint32_t vtcm_bytes,
                                        uint32_t cfg_bytes,uint32_t *out_cfg) {
    if (!out_cfg||!cfg_bytes||cfg_bytes>vtcm_bytes) return -1;
    const uint32_t cfg=(vtcm_bytes-cfg_bytes)&~255u;
    *out_cfg=cfg;
    return need<=cfg?0:-1;
}

#endif
