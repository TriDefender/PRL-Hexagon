/* Independent, static-HexKL HMX + HVX experiment. No SDKL/skel dependency.
 * X layout: [row-block64][slice16][kt4][row64][k32], u8=A'+128.
 * W input: [slice16][k128][column n], signed int8.
 * flags: 1=stage profiling; 2=retain HMX accumulator across slices (experimental);
 *        4=reload weights; 8=prepare weights only; 16=inline HMX mm;
 *        32=raw byte exports fused with HVX fold (retained mode only);
 *        64=HMX producer/HVX consumer pipeline (requires 32 and 2);
 *        128=corrected HMX state via complement MMs; 256=corrected export bias.
 *        4096=asynchronous user-DMA double-buffered input (multi only);
 *        8192=pack signed RM input on the DSP (multi only, excludes DMA);
 *        16384=one queue publication/completion per 64-column band (multi only).
 *        32768=adaptive exact signed-24 export (bias+compact+multi only).
 * 128 and 256 require raw retained mode and are mutually exclusive.
 * W may have length 0 when cached.
 * Output: exact transcript, followed by 64 diagnostic int32 words.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "fused.h"
#include "AEEStdErr.h"
#include "HAP_perf.h"
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"
#include "hexkl_micro.h"
#include "hmx_read_fast.h"
#include "fused_pipeline.h"
#include "fused_multi.h"
#include "fused_band.h"
#include "fused_corrected.h"
#include "fused_fold_compact.h"
#include "fused_dma.h"
#include "fused_signed_pack.h"
#include "fused_hash_impl.h"
#include "fused_adaptive_export.h"

#ifndef PRL_ASYNC_STAGE
#define PRL_ASYNC_STAGE 0
#endif
#if PRL_ASYNC_STAGE
#include "fused_stage.h"
#endif
#ifndef PRL_MM4_POINTER
#define PRL_MM4_POINTER 0
#endif
#if PRL_MM4_POINTER
#include "hmx_mm4.h"
#endif
#ifndef PRL_ADAPTIVE_SHARED
#define PRL_ADAPTIVE_SHARED 0
#endif
#if PRL_ADAPTIVE_SHARED
#include "fused_adaptive_shared.h"
#endif
#ifndef PRL_COMPACT_VTCM
#define PRL_COMPACT_VTCM 0
#endif
#if PRL_COMPACT_VTCM
#include "fused_layout_compact.h"
#endif

/* Validated hot-path specialization. Profiled calls and every feature
 * combination except the two normal compact pipelines keep the generic path. */
#ifndef PRL_SPECIALIZE_HOT
#define PRL_SPECIALIZE_HOT 0
#endif
#ifndef PRL_SPECIALIZE_QUEUE
#define PRL_SPECIALIZE_QUEUE 0
#endif
#if PRL_SPECIALIZE_QUEUE && !PRL_SPECIALIZE_HOT
#error PRL_SPECIALIZE_QUEUE requires PRL_SPECIALIZE_HOT
#endif
#if PRL_SPECIALIZE_QUEUE
#include "fused_multi_known.h"
#endif
#if PRL_SPECIALIZE_HOT
#define PRL_HOT_INLINE static inline __attribute__((always_inline))
#else
#define PRL_HOT_INLINE static inline
#endif

#include "fused_grouped.h"

#ifndef PRL_HMX_CLOCK_POLICY
#define PRL_HMX_CLOCK_POLICY 0
#endif
#if PRL_HMX_CLOCK_POLICY < 0 || PRL_HMX_CLOCK_POLICY > 4
#error PRL_HMX_CLOCK_POLICY must be 0, 1, 2, 3 or 4
#endif
#ifndef PRL_AUTO_WORKERS
#define PRL_AUTO_WORKERS 0
#endif
#if PRL_AUTO_WORKERS != 0 && PRL_AUTO_WORKERS != 1
#error PRL_AUTO_WORKERS must be 0 or 1
#endif
#if PRL_HMX_CLOCK_POLICY
#include "fused_power_vote.h"
static prl_hmx_vote g_clock_vote;
static unsigned g_clock_call_active;
#endif

static uint8_t *g_v;
static uint32_t g_size;
static int g_init = -1, g_n, g_retained, g_bias_ready, g_complement_ready;
static int g_pipeline_poisoned;
/* Immutable width flags for the complete cached retained-mode weight job. */
static uint8_t g_adaptive_short[16*64];
static int32_t g_adaptive_abs_prefix[2048];
static int g_adaptive_ready;
static uint32_t g_adaptive_short_count;
static uint32_t g_wh, g_dv, g_x, g_res, g_cum, g_cfg, g_scratch, g_ttmp, g_pipe_raw;
static uint32_t g_bias_configs,g_complement,g_constant_activation,g_constant_weight;
static uint32_t g_multi_raw,g_dma_x2,g_dma_descriptors;
#if PRL_ASYNC_STAGE
static inline int use_async_stage(int m,int flags) {
    /* A single row block cannot overlap its first input copy. Device A/B
     * showed a regression at m64 and gains at m512/m2048 for n64/n1024. */
    return m>=512&&(flags==2930||flags==35698);
}
#endif

/* Auto growth is confined to calls that already reserve the maximum four
 * raw rings for asynchronous stage, so it cannot increase live VTCM need. */
static unsigned selected_multi_workers(int m,int flags) {
#if PRL_AUTO_WORKERS && PRL_GROUP_WORKERS == 0 && PRL_HMX_CLOCK_POLICY > 0 && PRL_GROUPED_ENABLED && PRL_ASYNC_STAGE
    if (use_async_stage(m,flags)) return 4u;
#endif
    (void)m;
    return requested_multi_workers(flags);
}

static int initialize(void) {
    if (g_init == -1) {
        uint32_t rate;
        g_init = hexkl_micro_hw_init(&g_v, &g_size, &rate);
        if (!g_init) g_init = hexkl_micro_hmx_lock();
    }
    return g_init;
}
static uint32_t align2048(uint32_t n) { return (n + 2047u) & ~2047u; }
static void put64(uint32_t *o, int at, uint64_t x) {
    o[at] = (uint32_t)x; o[at+1] = (uint32_t)(x >> 32);
}
static inline HVX_Vector reduce_columns(HVX_Vector v) {
    v = Q6_V_vxor_VV(v, Q6_V_vlalign_VVR(v, v, 64));
    v = Q6_V_vxor_VV(v, Q6_V_vlalign_VVR(v, v, 32));
    return Q6_V_vxor_VV(v, Q6_V_valign_VVR(v, v, 4));
}
static __attribute__((noinline)) void store_scratch(uint32_t *tt, int combine) {
    /* All eight vector results have been written first. Avoid the long immediate
     * vector-store -> scalar-load RAW stall (V79 HVX PRM 3.9.7). Transcripts stay
     * in VTCM for a row-block, then leave through one contiguous vector copy. */
    const volatile uint32_t *p=(const volatile uint32_t *)(g_v+g_scratch);
    __asm__ volatile("dcfetch(%0)" :: "r"(p) : "memory");
#pragma clang loop unroll(disable)
    for (int rbi=0;rbi<8;rbi++,p+=32,tt+=64) {
        __asm__ volatile("dcfetch(%0)" :: "r"(p+32) : "memory");
        const uint32_t a=p[0],b=p[2],c=p[4],d=p[6];
        if (combine) { tt[0]^=a;tt[16]^=b;tt[32]^=c;tt[48]^=d; }
        else { tt[0]=a;tt[16]=b;tt[32]=c;tt[48]=d; }
    }
}
PRL_HOT_INLINE int mm_call(uint32_t act,uint32_t weight,int direct) {
    if (direct) {
        uint8_t *a=g_v+act,*w=g_v+weight;
        /* Exactly the successful path of micro_mm_u8i8 after entry validation.
         * All offsets have already been bounds/alignment checked by the caller. */
        __asm__ volatile("{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3) }"
                         :: "r"(a),"r"(w),"r"(31),"r"(896):"memory");
        return 0;
    }
    return hexkl_micro_hmx_mm_u8i8(g_v,act,weight);
}

/* acc_read_int32 already returns 64 row-major vectors of 32 int32 words.
 * Verified offline: SDKL copy_32b_to_submatrix is a scalar strided copy only.
 * volatile prevents compiler movement above the external HMX read call.
 */
static void fold_pair(int s, int nband, int rb, int band, uint32_t *tt) {
    const volatile HVX_Vector *a = (const volatile HVX_Vector *)(g_v+g_res);
    const volatile HVX_Vector *b = a+64;
    HVX_Vector *ca = (HVX_Vector *)(g_v+g_cum)+(size_t)band*128;
    HVX_Vector *cb = ca+64;
    const HVX_Vector da = *(HVX_Vector *)(g_v+g_dv+(size_t)(s*g_n+band*64)*4);
    const HVX_Vector db = *(HVX_Vector *)(g_v+g_dv+(size_t)(s*g_n+band*64+32)*4);
    volatile HVX_Vector *scratch=(volatile HVX_Vector *)(g_v+g_scratch);
#pragma clang loop unroll(disable)
    for (int rbi=0; rbi<8; rbi++) {
        HVX_Vector z = Q6_V_vzero();
        for (int u=0; u<8; u++) {
            const int r=rbi+8*u;
            HVX_Vector av=Q6_Vw_vsub_VwVw(a[r],da);
            HVX_Vector bv=Q6_Vw_vsub_VwVw(b[r],db);
            if (s) { av=Q6_Vw_vadd_VwVw(av,ca[r]); bv=Q6_Vw_vadd_VwVw(bv,cb[r]); }
            if (s!=15) { ca[r]=av; cb[r]=bv; }
            z=Q6_V_vxor_VV(z,Q6_V_vxor_VV(av,bv));
        }
        scratch[rbi]=reduce_columns(z);
    }
    store_scratch(tt+(size_t)band*512+s,0);
}
static void fold_retained(int s, int nband, int rb, int cb, uint32_t *tt) {
    const volatile HVX_Vector *a=(const volatile HVX_Vector *)(g_v+g_res);
    /* Prefix sum correction; HMX retains the raw cumulative u8*i8 product. */
    const HVX_Vector d=*(HVX_Vector *)(g_v+g_dv+(size_t)(s*g_n+cb*32)*4);
    /* One vector per (rbi,s), kept entirely in VTCM. The unused retained-mode
     * cum allocation is exactly large enough: 16 bands * 8 * 16 * 128 = 256KB.
     * Extracting individual words here causes costly vector/scalar hand-offs. */
    volatile HVX_Vector *scratch=(volatile HVX_Vector *)(g_v+g_cum)+(size_t)(cb/2)*128;
#pragma clang loop unroll(disable)
    for (int rbi=0;rbi<8;rbi++) {
        HVX_Vector z=Q6_V_vzero();
        for (int u=0;u<8;u++) z=Q6_V_vxor_VV(z,Q6_Vw_vsub_VwVw(a[rbi+8*u],d));
        HVX_Vector q=reduce_columns(z);
        const int off=rbi*16+s;
        if (cb&1) q=Q6_V_vxor_VV(q,scratch[off]);
        scratch[off]=q;
    }
    (void)nband;(void)rb;(void)tt;
}
static void fold_raw_retained(int s,int cb,const uint8_t *raw) {
    const HVX_Vector d=*(HVX_Vector *)(g_v+g_dv+(size_t)(s*g_n+cb*32)*4);
    HVX_Vector z0=Q6_V_vzero(),z1=Q6_V_vzero(),z2=Q6_V_vzero(),z3=Q6_V_vzero();
    HVX_Vector z4=Q6_V_vzero(),z5=Q6_V_vzero(),z6=Q6_V_vzero(),z7=Q6_V_vzero();
#pragma clang loop unroll(disable)
    for (int u=0;u<8;u++) {
        HVX_Vector a,b,c,e;
        prl_hmx_unpack_bytes4(raw,2*u,&a,&b,&c,&e);
        z0=Q6_V_vxor_VV(z0,Q6_Vw_vsub_VwVw(a,d));
        z1=Q6_V_vxor_VV(z1,Q6_Vw_vsub_VwVw(b,d));
        z2=Q6_V_vxor_VV(z2,Q6_Vw_vsub_VwVw(c,d));
        z3=Q6_V_vxor_VV(z3,Q6_Vw_vsub_VwVw(e,d));
        prl_hmx_unpack_bytes4(raw,2*u+1,&a,&b,&c,&e);
        z4=Q6_V_vxor_VV(z4,Q6_Vw_vsub_VwVw(a,d));
        z5=Q6_V_vxor_VV(z5,Q6_Vw_vsub_VwVw(b,d));
        z6=Q6_V_vxor_VV(z6,Q6_Vw_vsub_VwVw(c,d));
        z7=Q6_V_vxor_VV(z7,Q6_Vw_vsub_VwVw(e,d));
    }
    volatile HVX_Vector *dst=(volatile HVX_Vector *)(g_v+g_cum)+(size_t)(cb/2)*128+s;
#define SAVE_Z(k,z) do { \
        HVX_Vector q=reduce_columns(z); \
        if (cb&1) q=Q6_V_vxor_VV(q,dst[(k)*16]); \
        dst[(k)*16]=q; \
    } while (0)
    SAVE_Z(0,z0);SAVE_Z(1,z1);SAVE_Z(2,z2);SAVE_Z(3,z3);
    SAVE_Z(4,z4);SAVE_Z(5,z5);SAVE_Z(6,z6);SAVE_Z(7,z7);
#undef SAVE_Z
}
static inline HVX_Vector shuffle_lo(HVX_Vector a,HVX_Vector b,int width) {
    return Q6_V_lo_W(Q6_W_vshuff_VVR(b,a,-width));
}
static void flush_retained_band(uint32_t *out,int rb,int band,int nband) {
    const HVX_Vector *p=(const HVX_Vector *)(g_v+g_cum)+(size_t)band*128;
    HVX_Vector *dst=(HVX_Vector *)(out+((size_t)rb*nband+band)*512);
#pragma clang loop unroll(disable)
    for (int rbi=0;rbi<8;rbi++,p+=16,dst+=2) {
        /* 16 slice rows -> four transcript columns. Every shuffle follows the
         * little-endian vshuff pattern from HVX PRM; unused columns are dropped
         * at the first two stages and the final 64-byte interleave. */
        HVX_Vector a0=shuffle_lo(p[0],p[1],4),a1=shuffle_lo(p[2],p[3],4);
        HVX_Vector a2=shuffle_lo(p[4],p[5],4),a3=shuffle_lo(p[6],p[7],4);
        HVX_Vector a4=shuffle_lo(p[8],p[9],4),a5=shuffle_lo(p[10],p[11],4);
        HVX_Vector a6=shuffle_lo(p[12],p[13],4),a7=shuffle_lo(p[14],p[15],4);
        HVX_Vector b0=shuffle_lo(a0,a1,8),b1=shuffle_lo(a2,a3,8);
        HVX_Vector b2=shuffle_lo(a4,a5,8),b3=shuffle_lo(a6,a7,8);
        HVX_VectorPair c0=Q6_W_vshuff_VVR(b1,b0,-16);
        HVX_VectorPair c1=Q6_W_vshuff_VVR(b3,b2,-16);
        HVX_VectorPair d0=Q6_W_vshuff_VVR(Q6_V_lo_W(c1),Q6_V_lo_W(c0),-32);
        HVX_VectorPair d1=Q6_W_vshuff_VVR(Q6_V_hi_W(c1),Q6_V_hi_W(c0),-32);
        dst[0]=shuffle_lo(Q6_V_lo_W(d0),Q6_V_hi_W(d0),64);
        dst[1]=shuffle_lo(Q6_V_lo_W(d1),Q6_V_hi_W(d1),64);
    }
}

typedef struct {
    uint32_t *tt;
    int nband,flags;
    prl_hash_options hash;
} prl_fold_context;
static void init_fold_context(prl_fold_context *c,uint32_t *out,int nband,int flags,
                               const prl_hash_options *hash) {
    c->tt=out;c->nband=nband;c->flags=flags;
    if (hash) c->hash=*hash;
    else c->hash.output_mode=0;
}
static void pipeline_consume(void *context,int rb,int cb,int s,const uint8_t *raw) {
    prl_fold_context *c=(prl_fold_context *)context;
#if PRL_ADAPTIVE_SHARED
    if (c->flags&32768)
        prl_compact_fold_adaptive_shared(s,cb,raw,g_v+g_cum,g_adaptive_short[s*(g_n/32)+cb]);
    else
#endif
    if ((c->flags&32768) && g_adaptive_short[s*(g_n/32)+cb])
        prl_compact_fold_raw24(s,cb,raw,g_v+g_cum);
    else if (c->flags&2048) prl_compact_fold_raw(s,cb,raw,g_v+g_cum);
    else if (c->flags&(128|256)) prl_corrected_fold_raw(s,cb,raw,g_v+g_cum);
    else fold_raw_retained(s,cb,raw);
    if ((cb&1) && s==15) {
        if (c->hash.output_mode) prl_hash_flush_compact(&c->hash,c->tt,rb,cb/2,c->nband,g_v+g_cum);
        else if (c->flags&2048) prl_compact_flush_band(c->tt,rb,cb/2,c->nband,g_v+g_cum);
        else flush_retained_band(c->tt,rb,cb/2,c->nband);
    }
}
PRL_HOT_INLINE void read_corrected_snapshot(int flags,int s,int cb,uint32_t raw) {
    const uint32_t cfg=g_bias_configs+(s*(g_n/32)+cb)*PRL_HMX_BIAS32_CONFIG_BYTES;
    if ((flags&32768) && g_adaptive_short[s*(g_n/32)+cb])
        prl_hmx_read_corrected_bytes24_raw(g_v,cfg,raw);
    else prl_hmx_read_corrected_bytes_raw(g_v,cfg,raw);
}
#if PRL_SPECIALIZE_HOT
/* Thread callbacks cannot inherit constants stored in their context. Keep the
 * compact feature choice in the callback's code; hash output mode is a job
 * option and must remain dynamic for both run() and run_hash(). */
static __attribute__((noinline)) void pipeline_flush_compact(prl_fold_context *c,int rb,int cb) {
    if (c->hash.output_mode)
        prl_hash_flush_compact(&c->hash,c->tt,rb,cb/2,c->nband,g_v+g_cum);
    else prl_compact_flush_band(c->tt,rb,cb/2,c->nband,g_v+g_cum);
}
PRL_HOT_INLINE void pipeline_consume_compact(void *context,int rb,int cb,int s,
                                            const uint8_t *raw,int adaptive) {
    prl_fold_context *c=(prl_fold_context *)context;
#if PRL_ADAPTIVE_SHARED
    if (adaptive)
        prl_compact_fold_adaptive_shared(s,cb,raw,g_v+g_cum,g_adaptive_short[s*(g_n/32)+cb]);
    else prl_compact_fold_raw(s,cb,raw,g_v+g_cum);
#else
    if (adaptive && g_adaptive_short[s*(g_n/32)+cb])
        prl_compact_fold_raw24(s,cb,raw,g_v+g_cum);
    else prl_compact_fold_raw(s,cb,raw,g_v+g_cum);
#endif
    /* Only one callback in 32 flushes a band. Keeping its transpose/hash out of
     * the per-snapshot function avoids long vector lifetimes and stack spills. */
    if ((cb&1) && s==15) pipeline_flush_compact(c,rb,cb);
}
static void pipeline_consume_2930(void *context,int rb,int cb,int s,const uint8_t *raw) {
    pipeline_consume_compact(context,rb,cb,s,raw,0);
}
static void pipeline_consume_35698(void *context,int rb,int cb,int s,const uint8_t *raw) {
    pipeline_consume_compact(context,rb,cb,s,raw,1);
}
#endif
static int run_pipeline(int m,int n,int flags,const byte *X,uint32_t *tt,
                        uint32_t *dg,uint64_t *stage,uint64_t *mm,uint64_t *read,
                        uint64_t *fold,uint64_t *wait,const prl_hash_options *hash) {
    const int profile=flags&1,direct=flags&16,ncb=n/32;
    /* Keep callback context and transport alive until successful join. A join
     * error leaves ownership unresolved, so retain the heap block and prohibit
     * further calls from reusing any VTCM until this PD is reconstructed. */
    void *allocation=malloc(sizeof(prl_pipe)+sizeof(prl_fold_context)+63u);
    if (!allocation) return AEE_ENOMEMORY;
    prl_pipe *pipe=(prl_pipe *)(((uintptr_t)allocation+63u)&~(uintptr_t)63u);
    prl_fold_context *context=(prl_fold_context *)(pipe+1);
    init_fold_context(context,tt,n/64,flags,hash);
    int rc=prl_pipe_start(pipe,g_v,g_pipe_raw,(uint32_t)(m/64)*ncb*16u,
                           profile,pipeline_consume,context);
    if (rc) {
        dg[20]=1;
        dg[40]=__atomic_load_n(&pipe->lock_rc,__ATOMIC_ACQUIRE);
        dg[44]=__atomic_load_n(&pipe->status,__ATOMIC_ACQUIRE);
        dg[48]=__atomic_load_n(&pipe->unlock_rc,__ATOMIC_ACQUIRE);
        dg[52]=__atomic_load_n(&pipe->processed,__ATOMIC_ACQUIRE);
        dg[60]=pipe->join_rc;
        if (!dg[44]) dg[44]=pipe->join_status;
        if (pipe->created) g_pipeline_poisoned=1;
        else free(allocation);
        return rc;
    }
    uint32_t produced=0;
    uint64_t t=0;
    for (int rb=0;rb<m/64;rb++) {
        if (profile) t=HAP_perf_get_pcycles();
        HVX_Vector *vx=(HVX_Vector *)(g_v+g_x);
        const HVX_Vector *src=(const HVX_Vector *)(X+(size_t)rb*16*64*128);
        for (int i=0;i<16*64;i++) vx[i]=src[i];
        if (profile) *stage+=HAP_perf_get_pcycles()-t;
        for (int cb=0;cb<ncb;cb++) {
            hexkl_micro_hmx_acc_clear_int32();
            for (int s=0;s<16;s++) {
                if (profile) t=HAP_perf_get_pcycles();
                if (flags&128) prl_corrected_slice(g_v,(uint32_t)s*8192u,
                    g_wh+(s*ncb+cb)*4096u,g_complement+(s*ncb+cb)*4096u,
                    g_constant_activation,g_constant_weight);
                else for (int kt=0;kt<4;kt++) {
                        rc=mm_call((s*4+kt)*2048,g_wh+(s*ncb*4+cb*4+kt)*1024,direct);
                        if (rc) goto finish;
                    }
                if (profile) *mm+=HAP_perf_get_pcycles()-t;
                uint32_t raw;
                rc=prl_pipe_acquire(pipe,produced,&raw);
                if (rc) goto finish;
                if (profile) t=HAP_perf_get_pcycles();
                if (flags&256) read_corrected_snapshot(flags,s,cb,raw);
                else prl_hmx_read_bytes_raw(g_v,g_cfg,raw);
                prl_pipe_publish(pipe,produced,rb,cb,s);
                if (profile) *read+=HAP_perf_get_pcycles()-t;
                produced++;
            }
        }
    }
finish:
    {
        int end_rc=prl_pipe_finish(pipe,rc!=0);
        *fold=pipe->busy_cycles;
        *wait=pipe->producer_wait_cycles;
        dg[20]=1;
        dg[40]=__atomic_load_n(&pipe->lock_rc,__ATOMIC_ACQUIRE);
        dg[44]=__atomic_load_n(&pipe->status,__ATOMIC_ACQUIRE);
        dg[48]=__atomic_load_n(&pipe->unlock_rc,__ATOMIC_ACQUIRE);
        dg[52]=__atomic_load_n(&pipe->processed,__ATOMIC_ACQUIRE);
        dg[60]=pipe->join_rc;
        if (!dg[44]) dg[44]=pipe->join_status;
        if (pipe->created) g_pipeline_poisoned=1;
        else free(allocation);
        return rc?rc:end_rc;
    }
}

#if PRL_SPECIALIZE_HOT
#define PRL_MULTI_IMPLEMENTATION static inline __attribute__((always_inline))
#define PRL_MULTI_NAME run_multi_pipeline_impl
#define PRL_MULTI_CALLBACK_PARAMETER ,prl_multi_consume_fn consume,int known_queue
#define PRL_MULTI_CALLBACK consume
#else
#define PRL_MULTI_IMPLEMENTATION static
#define PRL_MULTI_NAME run_multi_pipeline
#define PRL_MULTI_CALLBACK_PARAMETER
#define PRL_MULTI_CALLBACK pipeline_consume
#endif
PRL_MULTI_IMPLEMENTATION int PRL_MULTI_NAME(int m,int n,int flags,const byte *X,uint32_t *tt,
                              uint32_t *dg,uint64_t *stage,uint64_t *mm,
                              uint64_t *read,uint64_t *fold,uint64_t *wait,const prl_hash_options *hash
                              PRL_MULTI_CALLBACK_PARAMETER) {
    const int profile=flags&1,direct=flags&16,ncb=n/32;
    const unsigned workers=selected_multi_workers(m,flags);
    prl_fold_context *context=(prl_fold_context *)malloc(sizeof(*context));
    if (!context) return AEE_ENOMEMORY;
    init_fold_context(context,tt,n/64,flags,hash);
    prl_multi *pool=prl_multi_alloc();
    if (!pool) {free(context);return AEE_ENOMEMORY;}
    int rc;
#if PRL_GROUPED_ENABLED
    const int grouped=prl_grouped_enabled(flags,known_queue);
    if (grouped) rc=prl_grouped_start(pool,g_v,g_multi_raw,m,n,workers,profile,
                                      PRL_MULTI_CALLBACK,context);
    else
#endif
    rc=prl_multi_start(pool,g_v,g_multi_raw,m,n,workers,profile,
                       PRL_MULTI_CALLBACK,context);
    if (rc) {
        int live=0;
        dg[20]=pool->workers;
        for (unsigned k=0;k<pool->workers;k++) {
            live|=pool->worker[k].created;
            dg[40+k]=__atomic_load_n(&pool->worker[k].lock_rc,__ATOMIC_ACQUIRE);
            dg[44+k]=__atomic_load_n(&pool->worker[k].status,__ATOMIC_ACQUIRE);
            dg[48+k]=__atomic_load_n(&pool->worker[k].unlock_rc,__ATOMIC_ACQUIRE);
            dg[52+k]=__atomic_load_n(&pool->worker[k].processed,__ATOMIC_ACQUIRE);
            dg[60+k]=pool->worker[k].join_rc;
            if (!dg[44+k]) dg[44+k]=pool->worker[k].join_status;
        }
        if (live) g_pipeline_poisoned=1;
        else free(context);
        prl_multi_free(pool);return rc;
    }
#if PRL_GROUPED_ENABLED
    if (grouped) dg[21]|=PRL_GROUPED_DIAG_BITS;
#endif
    prl_dma dma={0};
#if PRL_ASYNC_STAGE
    prl_stage *stager=NULL;
    const int use_stage=use_async_stage(m,flags);
#endif
    const int use_dma=flags&4096;
    uint64_t t=0;
    if (use_dma) {
        if (profile) t=HAP_perf_get_pcycles();
        rc=prl_dma_init(&dma,g_v+g_dma_descriptors);
        if (!rc) rc=prl_dma_submit(&dma,0,(const uint8_t *)X,g_v+g_x);
        if (profile) *stage+=HAP_perf_get_pcycles()-t;
        if (rc) goto finish;
    }
#if PRL_ASYNC_STAGE
    if (use_stage) {
        stager=prl_stage_alloc();
        if (!stager) {rc=AEE_ENOMEMORY;goto finish;}
        rc=prl_stage_start(stager,(const uint8_t *)X,g_v,g_x,g_dma_x2,(uint32_t)m/64u);
        if (rc) goto finish;
    }
#endif
    for (int rb=0;rb<m/64;rb++) {
        if (profile) t=HAP_perf_get_pcycles();
        uint32_t act=(use_dma&&(rb&1))?g_dma_x2:g_x;
#if PRL_ASYNC_STAGE
        if (use_stage) {
            rc=prl_stage_take(stager,(uint32_t)rb,&act);
            if (rc) goto finish;
        } else
#endif
        if (use_dma) {
            rc=prl_dma_drain(&dma);
            if (rc) goto finish;
            if (rb+1<m/64) {
                const uint32_t next=(rb&1)?g_x:g_dma_x2;
                rc=prl_dma_submit(&dma,(unsigned)(rb+1)&1u,
                    (const uint8_t *)X+(size_t)(rb+1)*PRL_DMA_BLOCK_BYTES,g_v+next);
                if (rc) goto finish;
            }
        } else if (flags&8192) {
            prl_pack_signed_rm_block(g_v+act,(const int8_t *)X,m,rb);
        } else {
            HVX_Vector *vx=(HVX_Vector *)(g_v+act);
            const HVX_Vector *src=(const HVX_Vector *)(X+(size_t)rb*PRL_DMA_BLOCK_BYTES);
            for (int i=0;i<16*64;i++) vx[i]=src[i];
        }
        if (profile) *stage+=HAP_perf_get_pcycles()-t;
        for (int cb=0;cb<ncb;cb++) {
#if PRL_SPECIALIZE_QUEUE
            /* Both columns of a band use its actual pool owner, including
             * n64 clipping and uneven three-consumer jobs. */
            prl_multi_worker *known_worker=NULL;
            if (known_queue) known_worker=&pool->worker[((unsigned)cb>>1)%pool->workers];
#endif
#if PRL_GROUPED_ENABLED
            uint32_t grouped_tag=0,grouped_raw=0;
#endif
            hexkl_micro_hmx_acc_clear_int32();
            for (int s=0;s<16;s++) {
                if (profile) t=HAP_perf_get_pcycles();
                if (flags&128) prl_corrected_slice(g_v,act+(uint32_t)s*8192u,
                    g_wh+(s*ncb+cb)*4096u,g_complement+(s*ncb+cb)*4096u,
                    g_constant_activation,g_constant_weight);
#if PRL_MM4_POINTER
                else if (direct) prl_hmx_mm4_pointer(g_v+act+(uint32_t)s*8192u,
                    g_v+g_wh+(uint32_t)(s*ncb+cb)*4096u);
#endif
                else for (int kt=0;kt<4;kt++) {
                        rc=mm_call(act+(s*4+kt)*2048,g_wh+(s*ncb*4+cb*4+kt)*1024,direct);
                        if (rc) goto finish;
                    }
                if (profile) *mm+=HAP_perf_get_pcycles()-t;
                unsigned owner;
                uint32_t tag,raw;
#if PRL_GROUPED_ENABLED
                if (grouped) {
                    if (((unsigned)s&(PRL_SNAPSHOT_GROUP-1u))==0u)
                        rc=prl_grouped_acquire_known(known_worker,&grouped_tag,&grouped_raw);
                    tag=grouped_tag;
                    raw=grouped_raw+((unsigned)s&(PRL_SNAPSHOT_GROUP-1u))*PRL_MULTI_SLOT_BYTES;
                } else
#endif
                {
#if PRL_SPECIALIZE_QUEUE
                    if (known_queue) rc=prl_multi_acquire_known_unprofiled(known_worker,&tag,&raw);
                    else
#endif
                    rc=prl_multi_acquire(pool,cb/2,&owner,&tag,&raw);
                }
                if (rc) goto finish;
                if (profile) t=HAP_perf_get_pcycles();
                if (flags&256) read_corrected_snapshot(flags,s,cb,raw);
                else prl_hmx_read_bytes_raw(g_v,g_cfg,raw);
#if PRL_GROUPED_ENABLED
                if (grouped) {
                    if (((unsigned)s&(PRL_SNAPSHOT_GROUP-1u))==PRL_SNAPSHOT_GROUP-1u)
                        prl_grouped_publish_known(known_worker,tag,rb,cb,s+1-PRL_SNAPSHOT_GROUP);
                } else
#endif
                {
#if PRL_SPECIALIZE_QUEUE
                    if (known_queue) prl_multi_publish_known(known_worker,tag,rb,cb,s);
                    else
#endif
                    prl_multi_publish(pool,owner,tag,rb,cb,s);
                }
                if (profile) *read+=HAP_perf_get_pcycles()-t;
            }
        }
#if PRL_ASYNC_STAGE
        if (use_stage) prl_stage_release(stager,(uint32_t)rb);
#endif
    }
finish:
    {
#if PRL_GROUPED_ENABLED
        /* Drain an unpublished partial group before abort permits VTCM reuse. */
        if (grouped&&rc) prl_multi_complete();
#endif
#if PRL_ASYNC_STAGE
        if (stager) {
            const int stage_rc=prl_stage_finish(stager,rc!=0);
            if (!rc) rc=stage_rc;
            if (pool->workers<=2u) {
                dg[42]=(uint32_t)__atomic_load_n(&stager->sync.lock_rc,__ATOMIC_ACQUIRE);
                dg[46]=(uint32_t)__atomic_load_n(&stager->sync.status,__ATOMIC_ACQUIRE);
                dg[50]=(uint32_t)__atomic_load_n(&stager->sync.unlock_rc,__ATOMIC_ACQUIRE);
                dg[54]=__atomic_load_n(&stager->sync.processed,__ATOMIC_ACQUIRE);
                dg[62]=(uint32_t)stager->sync.join_rc;
                if (!(flags&32768)) {dg[38]=0x53544147;dg[39]=dg[54];}
            } else if (!(flags&32768)) {
                /* Consumer 2 owns the original fixed stage-detail slots. */
                dg[38]=0x53544757; /* STGW: progress only, not STAG details. */
                dg[39]=__atomic_load_n(&stager->sync.processed,__ATOMIC_ACQUIRE);
            }
            if (stager->sync.created) g_pipeline_poisoned=1;
            else prl_stage_free(stager);
        }
#endif
        if (use_dma) {
            const uint64_t drain0=profile?HAP_perf_get_pcycles():0;
            const int dma_rc=prl_dma_drain(&dma);
            dg[38]=(uint32_t)dma.api_present;dg[39]=(uint32_t)dma.probe_rc;
            if (profile) *stage+=HAP_perf_get_pcycles()-drain0;
            if (!rc) rc=dma_rc;
            if (dma.active) g_pipeline_poisoned=1;
        }
        int end_rc=prl_multi_finish(pool,rc!=0);
        dg[20]=pool->workers;
        for (unsigned k=0;k<pool->workers;k++) {
            const uint64_t busy=pool->worker[k].busy_cycles;
            const uint64_t waiting=pool->worker[k].producer_wait_cycles;
            *fold+=busy;*wait+=waiting;
            put64(dg,22+2*k,busy);put64(dg,30+2*k,waiting);
            dg[40+k]=__atomic_load_n(&pool->worker[k].lock_rc,__ATOMIC_ACQUIRE);
            dg[44+k]=__atomic_load_n(&pool->worker[k].status,__ATOMIC_ACQUIRE);
            dg[48+k]=__atomic_load_n(&pool->worker[k].unlock_rc,__ATOMIC_ACQUIRE);
            dg[52+k]=__atomic_load_n(&pool->worker[k].processed,__ATOMIC_ACQUIRE);
            dg[60+k]=pool->worker[k].join_rc;
            if (!dg[44+k]) dg[44+k]=pool->worker[k].join_status;
        }
        int live=0;
        for (unsigned k=0;k<pool->workers;k++) live|=pool->worker[k].created;
        if (live) g_pipeline_poisoned=1;
        else free(context);
        prl_multi_free(pool);
        return rc?rc:end_rc;
    }
}
#undef PRL_MULTI_IMPLEMENTATION
#undef PRL_MULTI_NAME
#undef PRL_MULTI_CALLBACK_PARAMETER
#undef PRL_MULTI_CALLBACK

#if PRL_SPECIALIZE_HOT
static int run_multi_pipeline(int m,int n,int flags,const byte *X,uint32_t *tt,
                              uint32_t *dg,uint64_t *stage,uint64_t *mm,
                              uint64_t *read,uint64_t *fold,uint64_t *wait,const prl_hash_options *hash) {
    return run_multi_pipeline_impl(m,n,flags,X,tt,dg,stage,mm,read,fold,wait,hash,
                                   pipeline_consume,0);
}
static __attribute__((noinline)) int run_multi_pipeline_2930(int m,int n,const byte *X,uint32_t *tt,
                              uint32_t *dg,uint64_t *stage,uint64_t *mm,
                              uint64_t *read,uint64_t *fold,uint64_t *wait,const prl_hash_options *hash) {
    return run_multi_pipeline_impl(m,n,2930,X,tt,dg,stage,mm,read,fold,wait,hash,
                                   pipeline_consume_2930,PRL_SPECIALIZE_QUEUE);
}
static __attribute__((noinline)) int run_multi_pipeline_35698(int m,int n,const byte *X,uint32_t *tt,
                              uint32_t *dg,uint64_t *stage,uint64_t *mm,
                              uint64_t *read,uint64_t *fold,uint64_t *wait,const prl_hash_options *hash) {
    return run_multi_pipeline_impl(m,n,35698,X,tt,dg,stage,mm,read,fold,wait,hash,
                                   pipeline_consume_35698,PRL_SPECIALIZE_QUEUE);
}
#endif

static void band_consume(void *context,int rb,int band,int unused,const uint8_t *raw) {
    (void)unused;
    for (int half=0;half<2;half++) for (int slice=0;slice<16;slice++)
        pipeline_consume(context,rb,2*band+half,slice,
            raw+(half*16+slice)*PRL_MULTI_SLOT_BYTES);
}
static int run_band_pipeline(int m,int n,int flags,const byte *X,uint32_t *tt,
                              uint32_t *dg,uint64_t *stage,uint64_t *mm,
                              uint64_t *read,uint64_t *fold,uint64_t *wait,const prl_hash_options *hash) {
    const int profile=flags&1,direct=flags&16,ncb=n/32;
    const unsigned workers=selected_multi_workers(m,flags);
    prl_fold_context *context=(prl_fold_context *)malloc(sizeof(*context));
    if (!context) return AEE_ENOMEMORY;
    init_fold_context(context,tt,n/64,flags,hash);
    prl_band *pool=prl_band_alloc();
    if (!pool) {free(context);return AEE_ENOMEMORY;}
    int rc=prl_band_start(pool,g_v,g_multi_raw,m,n,workers,profile,
                           band_consume,context);
    if (rc) {
        int live=0;
        dg[20]=pool->workers;dg[21]=1; /* processed/produced unit: bands */
        for (unsigned k=0;k<pool->workers;k++) {
            live|=pool->worker[k].created;
            dg[40+k]=__atomic_load_n(&pool->worker[k].lock_rc,__ATOMIC_ACQUIRE);
            dg[44+k]=__atomic_load_n(&pool->worker[k].status,__ATOMIC_ACQUIRE);
            dg[48+k]=__atomic_load_n(&pool->worker[k].unlock_rc,__ATOMIC_ACQUIRE);
            dg[52+k]=__atomic_load_n(&pool->worker[k].processed,__ATOMIC_ACQUIRE);
            dg[60+k]=pool->worker[k].join_rc;
            if (!dg[44+k]) dg[44+k]=pool->worker[k].join_status;
        }
        if (live) g_pipeline_poisoned=1;
        else free(context);
        prl_band_free(pool);return rc;
    }
    prl_dma dma={0};
    const int use_dma=flags&4096;
    uint64_t t=0;
    if (use_dma) {
        if (profile) t=HAP_perf_get_pcycles();
        rc=prl_dma_init(&dma,g_v+g_dma_descriptors);
        if (!rc) rc=prl_dma_submit(&dma,0,(const uint8_t *)X,g_v+g_x);
        if (profile) *stage+=HAP_perf_get_pcycles()-t;
        if (rc) goto finish;
    }
    for (int rb=0;rb<m/64;rb++) {
        if (profile) t=HAP_perf_get_pcycles();
        const uint32_t act=(use_dma&&(rb&1))?g_dma_x2:g_x;
        if (use_dma) {
            rc=prl_dma_drain(&dma);
            if (rc) goto finish;
            if (rb+1<m/64) {
                const uint32_t next=(rb&1)?g_x:g_dma_x2;
                rc=prl_dma_submit(&dma,(unsigned)(rb+1)&1u,
                    (const uint8_t *)X+(size_t)(rb+1)*PRL_DMA_BLOCK_BYTES,g_v+next);
                if (rc) goto finish;
            }
        } else if (flags&8192) {
            prl_pack_signed_rm_block(g_v+act,(const int8_t *)X,m,rb);
        } else {
            HVX_Vector *vx=(HVX_Vector *)(g_v+act);
            const HVX_Vector *src=(const HVX_Vector *)(X+(size_t)rb*PRL_DMA_BLOCK_BYTES);
            for (int i=0;i<16*64;i++) vx[i]=src[i];
        }
        if (profile) *stage+=HAP_perf_get_pcycles()-t;
        for (int band=0;band<n/64;band++) {
            unsigned owner;
            uint32_t tag,raw_band;
            rc=prl_band_acquire(pool,band,&owner,&tag,&raw_band);
            if (rc) goto finish;
            for (int half=0;half<2;half++) {
                const int cb=2*band+half;
                hexkl_micro_hmx_acc_clear_int32();
                for (int slice=0;slice<16;slice++) {
                    if (profile) t=HAP_perf_get_pcycles();
                    if (flags&128) prl_corrected_slice(g_v,act+(uint32_t)slice*8192u,
                        g_wh+(slice*ncb+cb)*4096u,g_complement+(slice*ncb+cb)*4096u,
                        g_constant_activation,g_constant_weight);
                    else for (int kt=0;kt<4;kt++) {
                        rc=mm_call(act+(slice*4+kt)*2048,
                            g_wh+(slice*ncb*4+cb*4+kt)*1024,direct);
                        if (rc) goto finish;
                    }
                    if (profile) { *mm+=HAP_perf_get_pcycles()-t;t=HAP_perf_get_pcycles(); }
                    const uint32_t raw=raw_band+(half*16+slice)*PRL_MULTI_SLOT_BYTES;
                    if (flags&256) read_corrected_snapshot(flags,slice,cb,raw);
                    else prl_hmx_read_bytes_raw(g_v,g_cfg,raw);
                    if (profile) *read+=HAP_perf_get_pcycles()-t;
                }
            }
            if (profile) t=HAP_perf_get_pcycles();
            prl_band_publish(pool,owner,tag,rb,band,0);
            if (profile) *read+=HAP_perf_get_pcycles()-t;
        }
    }
finish:
    {
        if (use_dma) {
            const uint64_t drain0=profile?HAP_perf_get_pcycles():0;
            const int dma_rc=prl_dma_drain(&dma);
            dg[38]=(uint32_t)dma.api_present;dg[39]=(uint32_t)dma.probe_rc;
            if (profile) *stage+=HAP_perf_get_pcycles()-drain0;
            if (!rc) rc=dma_rc;
            if (dma.active) g_pipeline_poisoned=1;
        }
        int end_rc=prl_band_finish(pool,rc!=0);
        dg[20]=pool->workers;dg[21]=1; /* processed/produced unit: bands */
        for (unsigned k=0;k<pool->workers;k++) {
            const uint64_t busy=pool->worker[k].busy_cycles;
            const uint64_t waiting=pool->worker[k].producer_wait_cycles;
            *fold+=busy;*wait+=waiting;
            put64(dg,22+2*k,busy);put64(dg,30+2*k,waiting);
            dg[40+k]=__atomic_load_n(&pool->worker[k].lock_rc,__ATOMIC_ACQUIRE);
            dg[44+k]=__atomic_load_n(&pool->worker[k].status,__ATOMIC_ACQUIRE);
            dg[48+k]=__atomic_load_n(&pool->worker[k].unlock_rc,__ATOMIC_ACQUIRE);
            dg[52+k]=__atomic_load_n(&pool->worker[k].processed,__ATOMIC_ACQUIRE);
            dg[60+k]=pool->worker[k].join_rc;
            if (!dg[44+k]) dg[44+k]=pool->worker[k].join_status;
        }
        int live=0;
        for (unsigned k=0;k<pool->workers;k++) live|=pool->worker[k].created;
        if (live) g_pipeline_poisoned=1;
        else free(context);
        prl_band_free(pool);
        return rc?rc:end_rc;
    }
}

static int fused_run_core(int m,int n,int flags,const byte *X,int XLen,
                           const byte *Wk,int WkLen,uint32_t *tt,uint32_t *dg,
                           const prl_hash_options *hash,uint64 *cycles) {
    *cycles=0;
    if (m<64 || m>8192 || n<64 || n>2048 || m%64 || n%64 || flags<0 || flags>65535 ||
        ((flags&32)&&!(flags&2)) || ((flags&(64|128|256))&&((flags&34)!=34)) ||
        ((flags&384)==384) || ((flags&1536)&&!(flags&64)) ||
        ((flags&2048)&&!(flags&(128|256))) ||
        ((flags&28672)&&!(flags&1536)) || ((flags&12288)==12288) ||
        ((flags&32768) && (((flags&(2|32|64|256|2048))!=(2|32|64|256|2048)) || !(flags&1536))))
        return AEE_EBADPARM;
    if (g_pipeline_poisoned) return AEE_EFAILED;
    if (XLen<0 || WkLen<0) return AEE_EBADSIZE;
    memset(dg,0,64*4);
    int rc=initialize();
    if (rc) return rc;
    const uint64_t all0=HAP_perf_get_pcycles();
    const uint64_t all_ticks0=HAP_perf_get_qtimer_count();
    /* A nonempty W always starts a new job. Never combine stale WH with new
     * correction vectors when a caller forgets the explicit reload flag. */
    const int reload=WkLen||(flags&4)||g_n!=n;
    const uint32_t wh_size=16u*n*128u;
    const uint32_t dv_size=16u*n*4u;
    unsigned multi_workers=selected_multi_workers(m,flags);
    if (multi_workers>(unsigned)n/64u) multi_workers=(unsigned)n/64u;
    uint32_t need;
#if PRL_COMPACT_VTCM
    /* Policy 2 preserves every legacy-capable complement mode through n1600.
     * The selected offsets depend only on n, never on a call's feature bits. */
    const int compact_layout=PRL_COMPACT_VTCM==1 ||
        (PRL_COMPACT_VTCM==2&&n>=1664);
    if (compact_layout) {
        const prl_layout_compact_sizes sizes={
            PRL_PIPE_SLOTS*PRL_PIPE_SLOT_BYTES,PRL_HMX_BIAS32_CONFIG_BYTES,
            PRL_MULTI_RAW_BYTES(1),PRL_DMA_DESCRIPTOR_BYTES};
        prl_layout_compact l;
        if (prl_layout_compact_init(&l,(uint32_t)n,&sizes)) return AEE_EBADPARM;
        int use_stage=0;
#if PRL_ASYNC_STAGE
        use_stage=use_async_stage(m,flags);
#endif
        if (prl_layout_compact_need(&l,(uint32_t)flags,multi_workers,use_stage,&need))
            return AEE_EBADPARM;
        g_x=l.x;g_res=l.res;g_cum=l.cum;g_wh=l.wh;g_dv=l.dv;
        g_scratch=l.scratch;g_ttmp=l.ttmp;g_pipe_raw=l.pipe_raw;
        g_bias_configs=l.bias_configs;g_multi_raw=l.multi_raw;
        g_dma_x2=l.dma_x2;g_dma_descriptors=l.dma_descriptors;
        g_complement=l.complement;g_constant_activation=l.constant_activation;
        g_constant_weight=l.constant_weight;
    } else
#endif
    {
    g_x=0;                                      /* 16 slices of a 64-row block */
    g_res=16u*64u*128u;                          /* two 64x32 int32 tiles */
    g_cum=g_res+16384u;                          /* one row-block of cum */
    g_wh=align2048(g_cum+64u*n*4u);
    g_dv=align2048(g_wh+wh_size);
    g_scratch=align2048(g_dv+dv_size);
    g_ttmp=g_scratch+2048u;
    g_pipe_raw=align2048(g_ttmp+n*32u);
    g_bias_configs=align2048(g_pipe_raw+PRL_PIPE_SLOTS*PRL_PIPE_SLOT_BYTES);
    g_complement=align2048(g_bias_configs+16u*(n/32)*PRL_HMX_BIAS32_CONFIG_BYTES);
    g_constant_activation=align2048(g_complement+wh_size);
    g_constant_weight=g_constant_activation+2048u;
    g_multi_raw=align2048(g_constant_weight+2048u);
    /* Extra input/descriptor storage has a fixed location outside every raw
     * ring and all lazy weight caches. Enabling DMA never relocates a cache. */
    g_dma_x2=align2048(g_multi_raw+PRL_MULTI_RAW_BYTES(PRL_MULTI_MAX_WORKERS));
    g_dma_descriptors=g_dma_x2+PRL_DMA_BLOCK_BYTES;
    need=
#if PRL_ASYNC_STAGE
        use_async_stage(m,flags)?g_dma_x2+PRL_STAGE_BLOCK_BYTES:
#endif
        (flags&4096)?g_dma_descriptors+PRL_DMA_DESCRIPTOR_BYTES:
        multi_workers?g_multi_raw+PRL_MULTI_RAW_BYTES(multi_workers):
        (flags&128)?g_constant_weight+2048u:
        (flags&256)?g_complement:(flags&64)?g_bias_configs:g_ttmp+n*32u;
    }
    const uint32_t cfg_size=hexkl_micro_hmx_config_size();
    if (g_size<cfg_size) return AEE_ENOMEMORY;
    g_cfg=(g_size-cfg_size)&~255u;
#if PRL_COMPACT_VTCM
    /* Reserved diag21: bit0 keeps the transport unit; bit1 is actual compact
     * layout and high16 reports the live span in 2 KiB units. */
    const uint32_t layout_diag=((need/2048u)<<16)|(compact_layout?2u:0u);
    dg[1]=g_size;dg[2]=g_cfg;dg[21]=layout_diag;
#endif
    if (need>g_cfg) return AEE_ENOMEMORY;
    rc=hexkl_micro_hmx_setup_acc_read_int32(g_v,g_cfg);
    if (rc) return rc;
    if (reload) {
        g_n=0;
        g_bias_ready=g_complement_ready=0;g_adaptive_ready=0;g_adaptive_short_count=0;
        if ((size_t)WkLen<(size_t)16*n*128) return AEE_EBADSIZE;
        for (int s=0;s<16;s++) {
            const byte *w=Wk+(size_t)s*n*128;
            for (int cb=0;cb<n/32;cb++) for (int kt=0;kt<4;kt++) {
                rc=hexkl_micro_hmx_rm_to_wh_i8(g_v,g_wh+(s*(n/32)*4+cb*4+kt)*1024,(const int8_t *)w,kt,cb,n);
                if (rc) return rc;
            }
        }
    }
    /* Compute both forms for the selected mode; this is job preparation, not core time.
     * Kept in every non-empty-W call so switching mode uses the right correction. */
    if (WkLen) {
        if ((size_t)WkLen<(size_t)16*n*128) return AEE_EBADSIZE;
        int32_t *dv=(int32_t *)(g_v+g_dv);
        memset(g_adaptive_short,1,16u*(unsigned)(n/32));
        for (int s=0;s<16;s++) for (int j=0;j<n;j++) {
            int32_t sum=0,absolute=0;
            for (int k=0;k<128;k++) {
                const int32_t w=(int32_t)(int8_t)Wk[((size_t)s*128+k)*n+j];
                sum+=w;absolute+=w<0?-w:w;
            }
            dv[s*n+j]=128*sum+((flags&2)&&s ? dv[(s-1)*n+j] : 0);
            g_adaptive_abs_prefix[j]=absolute+(s?g_adaptive_abs_prefix[j]:0);
            if ((flags&2) && !prl_adaptive_s24_from_abs_sum(
                    g_adaptive_abs_prefix[j],dv[s*n+j]/128))
                g_adaptive_short[s*(n/32)+j/32]=0;
        }
        g_adaptive_ready=(flags&2)!=0;
        g_adaptive_short_count=0;
        for (int i=0;i<16*(n/32);i++) g_adaptive_short_count+=g_adaptive_short[i];
        g_n=n; /* publish the cache only after both WH and correction are ready */
        g_retained=flags&2;
    } else if (reload || g_retained!=(flags&2)) return AEE_EBADSIZE;
    /* Auxiliary caches are lazy and derive only from the complete cached job.
     * This supports empty-W mode switches without mixing old and new weights. */
    if ((flags&256) && !g_bias_ready) {
        const int ncb=n/32;
        int32_t *neg=(int32_t *)(g_v+g_scratch);
        for (int s=0;s<16;s++) for (int cb=0;cb<ncb;cb++) {
            const HVX_Vector d=*(const HVX_Vector *)(g_v+g_dv+(size_t)(s*n+cb*32)*4);
            *(HVX_Vector *)neg=Q6_Vw_vsub_VwVw(Q6_V_vzero(),d);
            prl_hmx_prepare_bias32_config(g_v+g_bias_configs+
                (s*ncb+cb)*PRL_HMX_BIAS32_CONFIG_BYTES,neg);
        }
        g_bias_ready=1;
    }
    if ((flags&128) && !g_complement_ready) {
        rc=prl_corrected_prepare(g_v,g_wh,wh_size,g_complement,
                                 g_constant_activation,g_constant_weight);
        if (rc) return rc;
        g_complement_ready=1;
    }
    if ((flags&32768) && !g_adaptive_ready) return AEE_EBADSIZE;
    uint64_t prep=HAP_perf_get_pcycles()-all0;
    if (flags&8) { dg[0]=0x46555344; dg[1]=g_size; dg[2]=g_cfg; put64(dg,4,prep); return 0; }
    if ((size_t)XLen<(size_t)16*m*128 || ((uintptr_t)X&127)) return AEE_EBADSIZE;
    uint64_t stage=0,mm=0,read=0,fold=0,wait=0,t;
    const int profile=flags&1,retained=flags&2,direct=flags&16,raw=flags&32,nband=n/64,ncb=n/32;
    const uint64_t core0=HAP_perf_get_pcycles();
    const uint64_t core_ticks0=HAP_perf_get_qtimer_count();
    if (flags&16384) {
        rc=run_band_pipeline(m,n,flags,X,tt,dg,&stage,&mm,&read,&fold,&wait,hash);
        if (rc) return rc;
    } else if (flags&1536) {
#if PRL_SPECIALIZE_HOT
        if (flags==2930)
            rc=run_multi_pipeline_2930(m,n,X,tt,dg,&stage,&mm,&read,&fold,&wait,hash);
        else if (flags==35698)
            rc=run_multi_pipeline_35698(m,n,X,tt,dg,&stage,&mm,&read,&fold,&wait,hash);
        else
#endif
        rc=run_multi_pipeline(m,n,flags,X,tt,dg,&stage,&mm,&read,&fold,&wait,hash);
        if (rc) return rc;
    } else if (flags&64) {
        rc=run_pipeline(m,n,flags,X,tt,dg,&stage,&mm,&read,&fold,&wait,hash);
        if (rc) return rc;
    } else for (int rb=0;rb<m/64;rb++) {
        uint32_t *tt_v=(uint32_t *)(g_v+g_ttmp);
        if (profile) t=HAP_perf_get_pcycles();
        /* 131KB contiguous: all 16 slice activation tiles remain resident. */
        HVX_Vector *vx=(HVX_Vector *)(g_v+g_x);
        const HVX_Vector *src=(const HVX_Vector *)(X+(size_t)rb*16*64*128);
        for (int i=0;i<16*64;i++) vx[i]=src[i];
        if (profile) stage+=HAP_perf_get_pcycles()-t;
        if (retained) {
            for (int cb=0;cb<ncb;cb++) {
                hexkl_micro_hmx_acc_clear_int32();
                for (int s=0;s<16;s++) {
                    if (profile) t=HAP_perf_get_pcycles();
                    if (flags&128) prl_corrected_slice(g_v,(uint32_t)s*8192u,
                        g_wh+(s*ncb+cb)*4096u,g_complement+(s*ncb+cb)*4096u,
                        g_constant_activation,g_constant_weight);
                    else for (int kt=0;kt<4;kt++) {
                            rc=mm_call((s*4+kt)*2048,g_wh+(s*ncb*4+cb*4+kt)*1024,direct);
                            if (rc) return rc;
                        }
                    if (profile) { mm+=HAP_perf_get_pcycles()-t; t=HAP_perf_get_pcycles(); }
                    if (raw) {
                        if (flags&256) read_corrected_snapshot(flags,s,cb,g_res);
                        else prl_hmx_read_bytes_raw(g_v,g_cfg,g_res);
                        __asm__ volatile("" ::: "memory");
                    } else {
                        rc=hexkl_micro_hmx_acc_read_int32(g_v,g_cfg,g_res);
                        if (rc) return rc;
                    }
                    if (profile) { read+=HAP_perf_get_pcycles()-t; t=HAP_perf_get_pcycles(); }
                    if (raw && (flags&2048)) prl_compact_fold_raw(s,cb,g_v+g_res,g_v+g_cum);
                    else if (raw && (flags&(128|256))) prl_corrected_fold_raw(s,cb,g_v+g_res,g_v+g_cum);
                    else if (raw) fold_raw_retained(s,cb,g_v+g_res);
                    else fold_retained(s,nband,rb,cb,tt_v);
                    if (profile) fold+=HAP_perf_get_pcycles()-t;
                }
                if (cb&1) {
                    if (profile) t=HAP_perf_get_pcycles();
                    if (flags&2048) prl_compact_flush_band(tt,rb,cb/2,nband,g_v+g_cum);
                    else flush_retained_band(tt,rb,cb/2,nband);
                    if (profile) fold+=HAP_perf_get_pcycles()-t;
                }
            }
        } else {
            for (int s=0;s<16;s++) for (int band=0;band<nband;band++) {
                for (int h=0;h<2;h++) {
                    if (profile) t=HAP_perf_get_pcycles();
                    hexkl_micro_hmx_acc_clear_int32();
                    for (int kt=0;kt<4;kt++) {
                        rc=mm_call((s*4+kt)*2048,g_wh+(s*ncb*4+(band*2+h)*4+kt)*1024,direct);
                        if (rc) return rc;
                    }
                    if (profile) { mm+=HAP_perf_get_pcycles()-t; t=HAP_perf_get_pcycles(); }
                    rc=hexkl_micro_hmx_acc_read_int32(g_v,g_cfg,g_res+h*8192);
                    if (rc) return rc;
                    if (profile) read+=HAP_perf_get_pcycles()-t;
                }
                if (profile) t=HAP_perf_get_pcycles();
                fold_pair(s,nband,rb,band,tt_v);
                if (profile) fold+=HAP_perf_get_pcycles()-t;
            }
        }
        if (!retained) {
            if (profile) t=HAP_perf_get_pcycles();
            HVX_Vector *dst=(HVX_Vector *)(tt+(size_t)rb*n*8);
            const HVX_Vector *tsrc=(const HVX_Vector *)tt_v;
            for (int i=0;i<n/4;i++) dst[i]=tsrc[i];
            if (profile) stage+=HAP_perf_get_pcycles()-t;
        }
    }
    *cycles=HAP_perf_get_pcycles()-core0;
    const uint64_t core_ticks=HAP_perf_get_qtimer_count()-core_ticks0;
    dg[0]=0x46555344; dg[1]=g_size; dg[2]=g_cfg; dg[3]=flags;
    put64(dg,4,prep); put64(dg,6,stage); put64(dg,8,mm); put64(dg,10,read); put64(dg,12,fold);
    put64(dg,14,*cycles); put64(dg,16,HAP_perf_get_pcycles()-all0);
    put64(dg,18,wait);
#if PRL_COMPACT_VTCM
    dg[21]|=layout_diag;
#endif
    if ((flags&32768) && !(flags&4096)) {
        dg[38]=g_adaptive_short_count;
        dg[39]=16u*(unsigned)(n/32); /* short snapshots/total per row-block */
    }
    put64(dg,56,core_ticks);
    put64(dg,58,HAP_perf_get_qtimer_count()-all_ticks0);
    return 0;
}

/* Power APIs wrap the complete core call, outside both its core and total
 * diagnostic timers. Only the inclusive RPC interval includes voting overhead. */
static inline __attribute__((always_inline))
int run_core_with_clock(int m,int n,int flags,const byte *X,int XLen,
                       const byte *Wk,int WkLen,uint32_t *tt,uint32_t *dg,
                       const prl_hash_options *hash,uint64 *cycles) {
#if PRL_HMX_CLOCK_POLICY
    unsigned expected=0;
    /* Guard initialize/cache preparation as well as the voted measured call.
     * A busy RPC must not mutate the active vote or another caller's output. */
    if (!__atomic_compare_exchange_n(&g_clock_call_active,&expected,1u,0,
                                     __ATOMIC_ACQ_REL,__ATOMIC_RELAXED))
        return AEE_EBADSTATE;
    memset(dg,0,64u*sizeof(*dg));
    int rc=AEE_EFAILED;
    if (g_pipeline_poisoned||g_clock_vote.poisoned||g_clock_vote.context) {
        g_pipeline_poisoned=1;
        goto release_guard;
    }
    rc=initialize(); /* Micro's power/client and hardware lock must exist first. */
    if (rc) goto release_guard;
    if (flags&8) {
        rc=fused_run_core(m,n,flags,X,XLen,Wk,WkLen,tt,dg,hash,cycles);
        prl_multi_complete();
        goto release_guard;
    }
    rc=prl_hmx_vote_begin(&g_clock_vote,PRL_HMX_CLOCK_POLICY);
    if (!rc) rc=fused_run_core(m,n,flags,X,XLen,Wk,WkLen,tt,dg,hash,cycles);
    /* Core errors include partial HMX exports and failed joins. Core finish
     * proves source_done/quiet before returning; retain its poison state. */
    prl_multi_complete();
    {
        const int end_rc=prl_hmx_vote_end(&g_clock_vote);
        if (!rc) rc=end_rc;
        if (end_rc||g_clock_vote.poisoned||g_clock_vote.context)
            g_pipeline_poisoned=1;
    }
    if (!rc&&g_pipeline_poisoned) rc=AEE_EFAILED;
    if (!rc) dg[21]|=((uint32_t)PRL_HMX_CLOCK_POLICY<<8)|(1u<<11);
release_guard:
    __atomic_store_n(&g_clock_call_active,0u,__ATOMIC_RELEASE);
    return rc;
#else
    return fused_run_core(m,n,flags,X,XLen,Wk,WkLen,tt,dg,hash,cycles);
#endif
}

/* Original full-transcript method: ABI, output layout and diagnostics unchanged. */
int prl_fused_fused_run(int m,int n,int flags,const byte *X,int XLen,
                        const byte *Wk,int WkLen,int *out,int outLen,uint64 *cycles) {
    if (!cycles) return AEE_EBADPARM;
    *cycles=0;
    if (m<64 || m>8192 || n<64 || n>2048 || m%64 || n%64) return AEE_EBADPARM;
    const size_t words=(size_t)(m/64)*(n/64)*512;
    if (!out || outLen<0 || (size_t)outLen<words+64 || ((uintptr_t)out&127))
        return AEE_EBADSIZE;
    return run_core_with_clock(m,n,flags,X,XLen,Wk,WkLen,(uint32_t *)out,
                           (uint32_t *)out+words,NULL,cycles);
}

/* Optional per-tile final-hash method. Fixed params are copied by QAIC into
 * scalar metadata. A 64-word diagnostic tail follows this mode's results. */
int prl_fused_fused_run_hash(int m,int n,int flags,int output_mode,
                             const byte *X,int XLen,const byte *Wk,int WkLen,
                             const prl_fused_fused_HashParams *params,
                             int *out,int outLen,
                             uint64 *cycles) {
    if (!cycles) return AEE_EBADPARM;
    *cycles=0;
    if (m<64 || m>8192 || n<64 || n>2048 || m%64 || n%64 ||
        (output_mode!=1 && output_mode!=2)) return AEE_EBADPARM;
    const int required=2|32|64|256|2048;
    if ((flags&required)!=required || !(flags&1536)) return AEE_EBADPARM;
    const size_t bands=(size_t)(m/64)*(n/64);
    const size_t need=bands*(output_mode==1?256:1);
    const int key_count=(m+2047)/2048;
    if (!out || !params || outLen<0 || (size_t)outLen<need+64 ||
        ((uintptr_t)out&(output_mode==1?127u:3u)))
        return AEE_EBADSIZE;
    prl_hash_options hash;
    hash.output_mode=output_mode;
    memset(hash.keys,0,sizeof(hash.keys));
    memcpy(hash.keys,params->keys,(size_t)key_count*32);
    memcpy(hash.bound,params->bound,sizeof(hash.bound));
    return run_core_with_clock(m,n,flags,X,XLen,Wk,WkLen,(uint32_t *)out,
                           (uint32_t *)out+need,&hash,cycles);
}
