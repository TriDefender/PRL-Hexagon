/*
 * qnpu_fused.c — production miner adapter for the fused Hexagon HMX path.
 *
 * The old QNN backend returns sixteen full INT32 matrices to ARM and folds
 * them there. This backend keeps the Pearl prefix folds and keyed hashes in
 * the DSP, returning only one 32-bit hit mask per 64x64 band. It implements
 * qnpu.h so the existing Stratum, nonce/job ownership, and PlainProof path can
 * be reused unchanged.
 */
#define _GNU_SOURCE
#include "qnpu.h"

#include <errno.h>
#include <pthread.h>
#include <remote.h>
#include <rpcmem.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fused.h"
#include "compound.h"
#include "protocol.h"
#include "noise_pack_neon.h"
#include "noise_compact_neon.h"
#include "qnpu_prep_pool.h"

/* HAP_POWER_ERR_UNSUPPORTED_API from the DSP's HAP_power.h; mirrored here so
 * the ARM side can accept the clock-unsupported diag marker. */
#define PRL_CLOCK_RC_UNSUPPORTED (-3)

#pragma weak remote_register_buf_attr2

#define FUSED_FLAGS 35698
#define FUSED_OUTPUT_MODE_MASK 2
#define FUSED_OUTPUT_MODE_DIGEST 1
#define COMPOUND_ATTEMPTS PRL_BATCH_ATTEMPTS
#define BYTEFOLD_RUNTIME_TAG_MAGIC UINT32_C(0x42464500)
#define BYTEFOLD_RUNTIME_TAG_MAGIC_MASK UINT32_C(0xffffff00)
#define BYTEFOLD_RUNTIME_TAG_WORD 63u
#if COMPOUND_ATTEMPTS != 32
#error "compact compound path requires exactly 32 attempts"
#endif
#define PREP_TASK_SINGLE 1
#define PREP_TASK_COMPOUND 2

enum { SLOT_FREE, SLOT_BUILDING, SLOT_READY, SLOT_EXECUTING };

typedef struct {
    uint8_t *x;
    int state;
    uint64_t nonce;
    uint8_t nonce8[8];
    pr_commitment commit;
    prl_fused_fused_HashParams params;
    double seed_ms;
    double pack_ms;
} fused_slot;

typedef struct {
    int valid;
    uint64_t nonce;
    uint8_t nonce8[8];
    pr_commitment commit;
    int hits;
    uint64_t coords[PRL_QNPU_MAXHITS];
    uint8_t digests[PRL_QNPU_MAXHITS * 32];
} fused_result;

typedef struct {
    uint64_t nonce;
    uint8_t nonce8[8];
    pr_commitment commit;
} compound_item;

typedef struct {
    uint8_t *x;                 /* shared base + 32 compact nonce records */
    int32_t *params;            /* 16 keys followed by one share bound */
    uint32_t *out;              /* 16 per-attempt mask arrays + summary */
    size_t out_words;
    int state;
    double prep_ms;
    uint64_t first_nonce;
    compound_item item[COMPOUND_ATTEMPTS];
} compound_slot;

struct pr_qnpu_ctx {
    uint32_t m, n, k;
    size_t alen, blen, xlen, bands, digest_words;
    size_t compound_input_bytes;
    int flags;
    uint64_t base_seed, macs;
    int weights_loaded;

    int8_t *ha, *hb;
    int8_t *access_a;
    pr_commitment job_commit;
    pr_commitment access_commit;
    pr_htree *tree;
    pr_u256 bound;

    int8_t *w;                 /* rpcmem [slice][k128][column] */
    uint32_t *out;              /* rpcmem; large enough for all-digest mode */
    size_t out_words;

    fused_slot slot[2];
    int prepared_slot;
    compound_slot compound[2];
    int prepared_compound_slot;
    int compound_result_slot;
    uint32_t compound_attempts;
    int compound_available;
    int compound_failed;
    int fused_failed;
    int compound_weights_loaded;
    int expected_bytefold_mode;
    int bytefold_mode_reported;
    pthread_t prep_thread;
    pthread_mutex_t prep_mu;
    pthread_cond_t prep_cv;
    int prep_thread_started;
    int prep_quit;
    int prep_task_kind;
    int prep_busy;
    int prep_task_slot;
    uint64_t prep_task_nonce;
    prl_prep_pool *prep_pool;

    fused_result pending;
    uint8_t *debug_ap_sm;
    double t_noise, t_exec, t_fold;
};

static pthread_once_t g_rpc_once = PTHREAD_ONCE_INIT;
static int g_rpc_rc = -1;
static int g_persist_map;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void init_rpc_once(void) {
    struct remote_rpc_control_unsigned_module req = {.domain = 3, .enable = 1};
    g_rpc_rc = remote_session_control(2, &req, sizeof(req));
    if (g_rpc_rc) return;
    rpcmem_init();
    const char *persist = getenv("PRL_PERSIST_MAP");
    g_persist_map = persist ? atoi(persist) != 0 : remote_register_buf_attr2 != NULL;
}

static int init_rpc(char *err, size_t cap) {
    pthread_once(&g_rpc_once, init_rpc_once);
    if (g_rpc_rc) {
        if (err && cap) snprintf(err, cap, "FastRPC unsigned session failed (%d)", g_rpc_rc);
        return -1;
    }
    return 0;
}

static int map_persistent(void *ptr, size_t size) {
    if (!g_persist_map) return 0;
    if (!remote_register_buf_attr2) return -1;
    const int fd = rpcmem_to_fd(ptr);
    if (fd < 0) return -1;
    remote_register_buf_attr2(ptr, size, -1, 0);
    remote_register_buf_attr2(ptr, size, fd,
                              FASTRPC_ATTR_KEEP_MAP | FASTRPC_ATTR_TRY_MAP_STATIC);
    return 0;
}

static void unmap_persistent(void *ptr, size_t size) {
    if (ptr && g_persist_map && remote_register_buf_attr2)
        remote_register_buf_attr2(ptr, size, -1, 0);
}

static void nonce56_bytes(uint64_t nonce, uint8_t out[8]) {
    /* Eight signed int7 values encode all 56 nonce bits without overlap. */
    for (unsigned j = 0; j < 8; ++j)
        out[j] = (uint8_t)((int)((nonce >> (7 * j)) & 127u) - 64);
}

static int pin_current_thread_rc(int cpu) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return EINVAL;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0 ? 0 : errno;
}

static void pin_current_thread(int cpu) {
    (void)pin_current_thread_rc(cpu);
}

static int prep_pool_pin(int cpu) {
    return pin_current_thread_rc(cpu);
}

static void legacy_nonce_bytes(uint64_t nonce, uint8_t out[8]) {
    /* Preserve the old QNN golden-selftest nonce encoding for --selftest. */
    for (unsigned j = 0; j < 8; ++j)
        out[j] = (uint8_t)((int)((nonce >> (8 * j)) & 127u) - 64);
}

static void fill_params(pr_qnpu_ctx *c, fused_slot *s) {
    memset(&s->params, 0, sizeof(s->params));
    memcpy(s->params.keys, s->commit.a_noise_seed.b, 32);
    memcpy(s->params.bound, c->bound.w, 32);
}

/* Called only by the preparation worker (or by the serial selftest path). */
static int prepare_slot(pr_qnpu_ctx *c, fused_slot *s, uint64_t nonce) {
    const double t0 = now_ms();
    nonce56_bytes(nonce, s->nonce8);
    s->nonce = nonce;

    uint8_t first_chunk[1024];
    memcpy(first_chunk, c->ha, sizeof(first_chunk));
    memcpy(first_chunk, s->nonce8, sizeof(s->nonce8));
    s->commit = c->job_commit;
    pr_htree_patch_chunk(c->tree, c->job_commit.job_key.b, 0, first_chunk);
    pr_htree_root(c->tree, s->commit.root_a.b);
    pr_bind_seeds(s->commit.root_a.b, s->commit.root_b.b,
                  c->m, c->n, &s->commit);
    fill_params(c, s);
    s->seed_ms = now_ms() - t0;

    const double p0 = now_ms();
    const int rc = prl_noise_pack_nonce_neon(s->commit.a_noise_seed.b,
                                               c->ha, c->m, s->nonce8, s->x);
    s->pack_ms = now_ms() - p0;
    return rc;
}

typedef struct {
    pr_qnpu_ctx *ctx;
    compound_slot *slot;
} compound_noise_job;

static int prepare_compound_noise(void *argument, unsigned attempt) {
    compound_noise_job *job = argument;
    pr_qnpu_ctx *c = job->ctx;
    compound_slot *s = job->slot;
    if (attempt >= c->compound_attempts) return EINVAL;
    compound_item *item = &s->item[attempt];
    return prl_noise_compact_record(item->commit.a_noise_seed.b, item->nonce8,
            s->x + PRL_COMPACT_BASE_BYTES + (size_t)attempt * PRL_COMPACT_RECORD_BYTES);
}

/* Merkle patching is serial because all attempts share the same tree. Once
 * commitments/keys are materialized, independent A' packing can use the
 * pinned ARM prep pool while the DSP executes the preceding RPC. */
static int prepare_compound_slot(pr_qnpu_ctx *c, compound_slot *s,
                                 uint64_t first_nonce) {
    const double t0 = now_ms();
    const uint64_t nonce_mask = UINT64_C(0x00ffffffffffffff);
    s->first_nonce = first_nonce & nonce_mask;
    for (unsigned attempt = 0; attempt < c->compound_attempts; ++attempt) {
        compound_item *item = &s->item[attempt];
        item->nonce = (first_nonce + attempt) & nonce_mask;
        nonce56_bytes(item->nonce, item->nonce8);
        uint8_t first_chunk[1024];
        memcpy(first_chunk, c->ha, sizeof(first_chunk));
        memcpy(first_chunk, item->nonce8, sizeof(item->nonce8));
        item->commit = c->job_commit;
        pr_htree_patch_chunk(c->tree, c->job_commit.job_key.b, 0, first_chunk);
        pr_htree_root(c->tree, item->commit.root_a.b);
        pr_bind_seeds(item->commit.root_a.b, item->commit.root_b.b,
                      c->m, c->n, &item->commit);
        memcpy(s->params + (size_t)attempt * 8u,
               item->commit.a_noise_seed.b, 32u);
    }
    memcpy(s->params + (size_t)c->compound_attempts * 8u,
           c->bound.w, 32u);
    compound_noise_job job = {.ctx=c, .slot=s};
    const int rc = prl_prep_pool_run(c->prep_pool, prepare_compound_noise, &job);
    s->prep_ms = now_ms() - t0;
    return rc;
}

static void *prep_worker(void *arg) {
    pr_qnpu_ctx *c = arg;
    pin_current_thread(7);
    for (;;) {
        pthread_mutex_lock(&c->prep_mu);
        while (!c->prep_quit && !c->prep_task_kind)
            pthread_cond_wait(&c->prep_cv, &c->prep_mu);
        if (c->prep_quit) {
            pthread_mutex_unlock(&c->prep_mu);
            return NULL;
        }
        const int kind = c->prep_task_kind;
        const int si = c->prep_task_slot;
        const uint64_t nonce = c->prep_task_nonce;
        c->prep_task_kind = 0;
        pthread_mutex_unlock(&c->prep_mu);

        int rc;
        if (kind == PREP_TASK_COMPOUND)
            rc = prepare_compound_slot(c, &c->compound[si], nonce);
        else
            rc = prepare_slot(c, &c->slot[si], nonce);

        pthread_mutex_lock(&c->prep_mu);
        if (kind == PREP_TASK_COMPOUND)
            c->compound[si].state = rc ? SLOT_FREE : SLOT_READY;
        else
            c->slot[si].state = rc ? SLOT_FREE : SLOT_READY;
        c->prep_busy = 0;
        pthread_cond_broadcast(&c->prep_cv);
        pthread_mutex_unlock(&c->prep_mu);
        if (rc) fprintf(stderr, "[prl-miner] %s input preparation failed (%d)\n",
                        kind == PREP_TASK_COMPOUND ? "compound" : "fused", rc);
    }
}

static int schedule_prep(pr_qnpu_ctx *c, int si, uint64_t nonce) {
    pthread_mutex_lock(&c->prep_mu);
    if (c->prep_quit || c->prep_task_kind || c->prep_busy || c->slot[si].state != SLOT_FREE) {
        pthread_mutex_unlock(&c->prep_mu);
        return -1;
    }
    c->slot[si].state = SLOT_BUILDING;
    c->prep_task_slot = si;
    c->prep_task_nonce = nonce;
    c->prep_busy = 1;
    c->prep_task_kind = PREP_TASK_SINGLE;
    pthread_cond_broadcast(&c->prep_cv);
    pthread_mutex_unlock(&c->prep_mu);
    return 0;
}

static int schedule_compound_prep(pr_qnpu_ctx *c, int si, uint64_t nonce) {
    pthread_mutex_lock(&c->prep_mu);
    if (c->prep_quit || c->prep_task_kind || c->prep_busy ||
        c->compound[si].state != SLOT_FREE) {
        pthread_mutex_unlock(&c->prep_mu);
        return -1;
    }
    c->compound[si].state = SLOT_BUILDING;
    c->prep_task_slot = si;
    c->prep_task_nonce = nonce;
    c->prep_busy = 1;
    c->prep_task_kind = PREP_TASK_COMPOUND;
    pthread_cond_broadcast(&c->prep_cv);
    pthread_mutex_unlock(&c->prep_mu);
    return 0;
}

static int wait_slot_ready(pr_qnpu_ctx *c, int si) {
    pthread_mutex_lock(&c->prep_mu);
    while (c->slot[si].state == SLOT_BUILDING && !c->prep_quit)
        pthread_cond_wait(&c->prep_cv, &c->prep_mu);
    const int ready = c->slot[si].state == SLOT_READY;
    pthread_mutex_unlock(&c->prep_mu);
    return ready ? 0 : -1;
}

static void slot_free(pr_qnpu_ctx *c, int si) {
    pthread_mutex_lock(&c->prep_mu);
    c->slot[si].state = SLOT_FREE;
    pthread_cond_broadcast(&c->prep_cv);
    pthread_mutex_unlock(&c->prep_mu);
}

static int wait_compound_ready(pr_qnpu_ctx *c, int si) {
    pthread_mutex_lock(&c->prep_mu);
    while (c->compound[si].state == SLOT_BUILDING && !c->prep_quit)
        pthread_cond_wait(&c->prep_cv, &c->prep_mu);
    const int ready = c->compound[si].state == SLOT_READY;
    pthread_mutex_unlock(&c->prep_mu);
    return ready ? 0 : -1;
}

static void compound_slot_free(pr_qnpu_ctx *c, int si) {
    pthread_mutex_lock(&c->prep_mu);
    c->compound[si].state = SLOT_FREE;
    pthread_cond_broadcast(&c->prep_cv);
    pthread_mutex_unlock(&c->prep_mu);
}

static int fused_run(pr_qnpu_ctx *c, fused_slot *s, int output_mode,
                     uint64 *cycles_out) {
    const size_t result_words = c->bands *
        (output_mode == FUSED_OUTPUT_MODE_DIGEST ? 256u : 1u);
    const int weight_len = c->weights_loaded ? 0 : (int)c->blen;
    uint64 cycles = 0;
    const double t0 = now_ms();
    const int rc = prl_fused_fused_run_hash(
        (int)c->m, (int)c->n, c->flags, output_mode,
        (const byte *)s->x, (int)c->xlen,
        (const byte *)c->w, weight_len, &s->params,
        (int *)c->out, (int)(result_words + 64u), &cycles);
    c->t_exec = now_ms() - t0;
    if (rc) {
        fprintf(stderr, "[prl-miner] fused DSP RPC failed rc=%d (0x%x)\n", rc, (unsigned)rc);
        return rc;
    }
    c->weights_loaded = 1;
    if (!cycles || c->out[result_words] != UINT32_C(0x46555344)) {
        fprintf(stderr, "[prl-miner] fused DSP RPC returned invalid diagnostics\n");
        return -1;
    }
    if (cycles_out) *cycles_out = cycles;
    return 0;
}

static uint64_t compound_diag_u64(const uint32_t *d, unsigned word) {
    return (uint64_t)d[word] | ((uint64_t)d[word + 1] << 32);
}

static unsigned compound_prep_workers(void) {
    const char *setting = getenv("PRL_COMPOUND_PREP_WORKERS");
    if (!setting || !*setting) return 4;
    const int workers = atoi(setting);
    return workers >= 1 && workers <= 4 ? (unsigned)workers : 4u;
}

static int parse_expected_bytefold_mode(int *mode) {
    const char *setting = getenv("PRL_EXPECT_BYTEFOLD_EARLY");
    if (!setting || !mode) return -1;
    if (setting[0] == '0' && setting[1] == '\0') *mode = 0;
    else if (setting[0] == '1' && setting[1] == '\0') *mode = 1;
    else return -1;
    return 0;
}

static int decode_bytefold_runtime_tag(uint32_t tag, unsigned *mode) {
    if ((tag & BYTEFOLD_RUNTIME_TAG_MAGIC_MASK) != BYTEFOLD_RUNTIME_TAG_MAGIC ||
        (tag & 0xffu) > 1u) return 0;
    *mode = tag & 0xffu;
    return 1;
}

/* A clock diag slot is either a real query (rc=0, Hz>0) or the DSP-side
 * "unsupported" marker used by pre-v79 firmware whose loader rejects any
 * ELF importing HAP_power_get (HAP_POWER_ERR_UNSUPPORTED_API, Hz=0). */
static int clock_slot_ok(uint32_t rc_w, uint32_t hz_w) {
    if (rc_w == 0u) return hz_w != 0u;
    return (int32_t)rc_w == PRL_CLOCK_RC_UNSUPPORTED && hz_w == 0u;
}

static int compound_summary_valid(pr_qnpu_ctx *c, const compound_slot *s,
                                  int rc, uint64_t cycles) {
    if (rc) {
        const uint32_t *d = s->out + (size_t)c->bands * c->compound_attempts;
        fprintf(stderr, "[prl-miner] compound DSP RPC failed rc=%d (0x%x); payload ignored\n"
                "  diag: ver=%u n=%u flags=%x init=%u core=%u clocks=%u/%u/%u/%u tag=%08x\n",
                rc, (unsigned)rc, d[CB_VERSION], d[CB_N], d[CB_FLAGS],
                d[CB_INIT_RC], d[CB_CORE_RC], d[CB_PRE_Q6_RC], d[CB_PRE_HMX_RC],
                d[CB_POST_Q6_RC], d[CB_POST_HMX_RC], d[CB_RUNTIME_KERNEL_TAG]);
        return 0;
    }
    const size_t words = c->bands * c->compound_attempts;
    const uint32_t *d = s->out + words;
    const uint32_t chunks = (c->compound_attempts + PRL_COMPOUND_MAX_CHUNK - 1u) /
                            PRL_COMPOUND_MAX_CHUNK;
    if (!cycles || d[CB_MAGIC] != PRL_COMPOUND_MAGIC ||
        d[CB_VERSION] != PRL_COMPOUND_VERSION ||
        d[CB_ATTEMPTS] != c->compound_attempts || d[CB_N] != c->n ||
        d[CB_FLAGS] != (uint32_t)(c->flags | PRL_COMPACT_FLAG) || d[CB_MODE] != FUSED_OUTPUT_MODE_MASK ||
        d[CB_CHUNKS_PLANNED] != chunks || d[CB_CHUNKS_DONE] != chunks ||
        d[CB_ATTEMPTS_DONE] != c->compound_attempts || d[CB_POISON] ||
        d[CB_CORE_RC] || d[CB_INIT_RC] || d[CB_BEGIN_RC] || d[CB_END_RC] ||
        d[CB_POLICY] != 1 || d[CB_PREPARE] ||
        !clock_slot_ok(d[CB_PRE_Q6_RC], d[CB_PRE_Q6_HZ]) ||
        !clock_slot_ok(d[CB_PRE_HMX_RC], d[CB_PRE_HMX_HZ]) ||
        !clock_slot_ok(d[CB_POST_Q6_RC], d[CB_POST_Q6_HZ]) ||
        !clock_slot_ok(d[CB_POST_HMX_RC], d[CB_POST_HMX_HZ]) ||
        d[CB_OUTPUT_WORDS] != words || d[CB_WORDS_PER_ATTEMPT] != c->bands ||
        d[CB_KERNEL_POISON] || d[CB_MAX_CHUNK] != PRL_COMPOUND_MAX_CHUNK ||
        compound_diag_u64(d, CB_OUTER_PCYCLES) != cycles ||
        !compound_diag_u64(d, CB_CORE_TICKS) || !compound_diag_u64(d, CB_OUTER_TICKS)) {
        fprintf(stderr, "[prl-miner] compound DSP summary invalid: done=%u/%u attempts=%u poison=%u rc=%u/%u/%u/%u\n",
                d[CB_CHUNKS_DONE], d[CB_CHUNKS_PLANNED], d[CB_ATTEMPTS_DONE],
                d[CB_POISON], d[CB_CORE_RC], d[CB_INIT_RC], d[CB_BEGIN_RC], d[CB_END_RC]);
        return 0;
    }
    const uint32_t runtime_tag = d[BYTEFOLD_RUNTIME_TAG_WORD];
    unsigned runtime_mode = 0;
    if (!decode_bytefold_runtime_tag(runtime_tag, &runtime_mode)) {
        fprintf(stderr, "[prl-miner] compound DSP runtime bytefold tag invalid tag=0x%08x\n",
                runtime_tag);
        return 0;
    }
    if (c->expected_bytefold_mode < 0 ||
        runtime_mode != (unsigned)c->expected_bytefold_mode) {
        fprintf(stderr,
                "[prl-miner] compound DSP runtime bytefold mode mismatch expected=%d actual=%u tag=0x%08x\n",
                c->expected_bytefold_mode, runtime_mode, runtime_tag);
        return 0;
    }
    if (!c->bytefold_mode_reported) {
        fprintf(stderr,
                "[prl-miner] compound runtime_deep4_mode=1 runtime_prefetch_order=0 runtime_bytefold_early=%u runtime_bytefold_tag=0x%08x expected=%d\n",
                runtime_mode, runtime_tag, c->expected_bytefold_mode);
        c->bytefold_mode_reported = 1;
    }
    return 1;
}

static int compound_run(pr_qnpu_ctx *c, compound_slot *s) {
    const size_t words = c->bands * c->compound_attempts;
    const int weight_len = c->compound_weights_loaded ? 0 : (int)c->blen;
    uint64 cycles = 0;
    const double t0 = now_ms();
    const int rc = prl_compound_batch_run(
        (int)c->compound_attempts, (int)c->n, c->flags | PRL_COMPACT_FLAG,
        FUSED_OUTPUT_MODE_MASK, (const byte *)s->x,
        (int)c->compound_input_bytes, (const byte *)c->w,
        weight_len, s->params, (int)(c->compound_attempts * 8u + 8u),
        (int *)s->out, (int)(words + PRL_COMPOUND_DIAG_WORDS), &cycles);
    c->t_exec = now_ms() - t0;
    if (!compound_summary_valid(c, s, rc, cycles)) return rc ? rc : -1;
    c->compound_weights_loaded = 1;
    c->macs += (uint64_t)c->m * c->n * c->k * c->compound_attempts;
    static unsigned report_batches;
    if ((++report_batches & 63u) == 0) {
        const uint32_t *d = s->out + words;
        const uint64_t core_pcycles = compound_diag_u64(d, CB_SUM_CORE_PCYCLES);
        fprintf(stderr,
            "[prl-miner] compound batch%u prep %.2f ms, RPC %.2f ms, core %.2f Gcycles, HMX %u->%u MHz, Q6 %u->%u MHz\n",
            c->compound_attempts, s->prep_ms, c->t_exec,
            (double)core_pcycles / 1.0e9,
            d[CB_PRE_HMX_HZ] / 1000000u, d[CB_POST_HMX_HZ] / 1000000u,
            d[CB_PRE_Q6_HZ] / 1000000u, d[CB_POST_Q6_HZ] / 1000000u);
    }
    return 0;
}

static void decode_masks_from(pr_qnpu_ctx *c, const uint32_t *mask_words,
                              fused_result *r);

int pr_qnpu_compound_available(pr_qnpu_ctx *c) {
    return c && c->compound_available && !c->compound_failed;
}

int pr_qnpu_fused_failed(const pr_qnpu_ctx *c) {
    return !c || c->fused_failed;
}

uint32_t pr_qnpu_compound_attempts(pr_qnpu_ctx *c) {
    return pr_qnpu_compound_available(c) ? c->compound_attempts : 0;
}

/* Prepare the following batch before entering the current compound RPC. */
int pr_qnpu_compound_pipe(pr_qnpu_ctx *c, uint64_t first_nonce) {
    if (!pr_qnpu_compound_available(c) || !c->tree) return -1;
    static int main_pinned;
    if (!main_pinned) {
        (void)pin_current_thread_rc(3);
        main_pinned = 1;
    }

    if (c->compound_result_slot >= 0) {
        compound_slot_free(c, c->compound_result_slot);
        c->compound_result_slot = -1;
    }

    const uint64_t nonce_mask = UINT64_C(0x00ffffffffffffff);
    first_nonce &= nonce_mask;
    int cur = c->prepared_compound_slot;
    if (cur < 0) {
        cur = 0;
        if (schedule_compound_prep(c, cur, first_nonce) || wait_compound_ready(c, cur)) {
            c->compound_failed = 1;
            fprintf(stderr, "[prl-miner] compound input prime failed\n");
            return -1;
        }
    } else if (wait_compound_ready(c, cur) ||
               c->compound[cur].first_nonce != first_nonce) {
        fprintf(stderr, "[prl-miner] compound nonce pipeline mismatch; rebuilding at %llu\n",
                (unsigned long long)first_nonce);
        compound_slot_free(c, cur);
        c->prepared_compound_slot = -1;
        if (schedule_compound_prep(c, cur, first_nonce) || wait_compound_ready(c, cur)) {
            c->compound_failed = 1;
            return -1;
        }
    }
    c->prepared_compound_slot = -1;
    c->compound[cur].state = SLOT_EXECUTING;

    const int next = 1 - cur;
    const uint64_t next_nonce = (first_nonce + c->compound_attempts) & nonce_mask;
    if (schedule_compound_prep(c, next, next_nonce) == 0)
        c->prepared_compound_slot = next;

    const int rc = compound_run(c, &c->compound[cur]);
    if (rc) {
        c->compound_failed = 1;
        compound_slot_free(c, cur);
        /* A failed compound namespace may be sticky-poisoned. Wait for and
         * release its speculative ARM input before falling back to fused RPC. */
        pthread_mutex_lock(&c->prep_mu);
        while (c->prep_busy) pthread_cond_wait(&c->prep_cv, &c->prep_mu);
        pthread_mutex_unlock(&c->prep_mu);
        if (c->prepared_compound_slot >= 0)
            compound_slot_free(c, c->prepared_compound_slot);
        c->prepared_compound_slot = -1;
        return rc;
    }
    c->compound[cur].state = SLOT_READY;
    c->compound_result_slot = cur;
    return 0;
}

int pr_qnpu_compound_result(pr_qnpu_ctx *c, uint32_t attempt,
                            uint64_t *nonce_out, uint64_t *coords,
                            uint8_t *digests) {
    if (!c) return -1;
    if (c->compound_result_slot < 0 || attempt >= c->compound_attempts) {
        c->compound_failed = 1;
        return -1;
    }
    compound_slot *s = &c->compound[c->compound_result_slot];
    compound_item *item = &s->item[attempt];
    c->access_commit = item->commit;
    memcpy(c->access_a, item->nonce8, sizeof(item->nonce8));
    if (nonce_out) *nonce_out = item->nonce;
    const uint32_t *masks = s->out + (size_t)attempt * c->bands;
    fused_result r = {0};
    r.commit = item->commit;
    r.nonce = item->nonce;
    memcpy(r.nonce8, item->nonce8, sizeof(r.nonce8));
    decode_masks_from(c, masks, &r);
    if (coords && r.hits)
        memcpy(coords, r.coords, (size_t)r.hits * sizeof(r.coords[0]));
    if (digests && r.hits)
        memset(digests, 0, (size_t)r.hits * 32u);
    return r.hits;
}

static void publish_result(pr_qnpu_ctx *c, const fused_result *r,
                           uint64_t *coords, uint8_t *digests) {
    c->access_commit = r->commit;
    memcpy(c->access_a, r->nonce8, sizeof(r->nonce8));
    if (coords && r->hits)
        memcpy(coords, r->coords, (size_t)r->hits * sizeof(r->coords[0]));
    if (digests && r->hits)
        memcpy(digests, r->digests, (size_t)r->hits * 32u);
}

static void decode_masks_from(pr_qnpu_ctx *c, const uint32_t *mask_words,
                              fused_result *r) {
    const uint32_t nband = c->n / 64u;
    r->hits = 0;
    for (size_t b = 0; b < c->bands; ++b) {
        uint32_t bits = mask_words[b];
        const uint32_t rb = (uint32_t)(b / nband);
        const uint32_t cb = (uint32_t)(b % nband);
        while (bits && r->hits < PRL_QNPU_MAXHITS) {
            const unsigned lane = (unsigned)__builtin_ctz(bits);
            bits &= bits - 1;
            const uint32_t tr = rb * 64u + lane / 4u;
            const uint32_t tc = cb * 64u + 2u * (lane % 4u);
            r->coords[r->hits] = (uint64_t)tr | ((uint64_t)tc << 32);
            memset(r->digests + (size_t)r->hits * 32u, 0, 32u);
            ++r->hits;
        }
        if (!bits) continue;
        /* The share API caps hits at 1024. Discard any excess mask bits. */
        break;
    }
}

static void decode_masks(pr_qnpu_ctx *c, fused_result *r) {
    decode_masks_from(c, c->out, r);
}

static void decode_all_digests(pr_qnpu_ctx *c, fused_slot *s, fused_result *r) {
    const uint32_t nband = c->n / 64u;
    r->hits = 0;
    for (size_t b = 0; b < c->bands && r->hits < PRL_QNPU_MAXHITS; ++b) {
        const uint32_t rb = (uint32_t)(b / nband);
        const uint32_t cb = (uint32_t)(b % nband);
        for (unsigned lane = 0; lane < 32 && r->hits < PRL_QNPU_MAXHITS; ++lane) {
            uint32_t words[8];
            for (unsigned w = 0; w < 8; ++w)
                words[w] = c->out[b * 256u + (size_t)w * 32u + lane];
            pr_u256 d;
            memcpy(&d, words, sizeof(words));
            if (pr_u256_cmp(&d, &c->bound) > 0) continue;
            const uint32_t tr = rb * 64u + lane / 4u;
            const uint32_t tc = cb * 64u + 2u * (lane % 4u);
            r->coords[r->hits] = (uint64_t)tr | ((uint64_t)tc << 32);
            memcpy(r->digests + (size_t)r->hits * 32u, words, sizeof(words));
            ++r->hits;
        }
    }
    r->commit = s->commit;
    memcpy(r->nonce8, s->nonce8, sizeof(r->nonce8));
    r->nonce = s->nonce;
    r->valid = 1;
}

static int setup_job_weights(pr_qnpu_ctx *c, char *err, size_t errcap) {
    pr_attempt_mats am = {.m=c->m, .n=c->n, .k=c->k,
        .a=c->ha, .bt=c->hb};
    am.ap = malloc(c->alen);
    am.btp = malloc(c->blen);
    am.a16 = malloc(c->alen * sizeof(int16_t));
    am.bt16 = malloc(c->blen * sizeof(int16_t));
    if (!am.ap || !am.btp || !am.a16 || !am.bt16) {
        if (err && errcap) snprintf(err, errcap, "job noise allocation failed");
        free(am.ap); free(am.btp); free(am.a16); free(am.bt16);
        return -1;
    }
    pr_noise_operands(&am, &c->job_commit);
    for (uint32_t s = 0; s < 16; ++s)
        for (uint32_t j = 0; j < c->n; ++j)
            for (uint32_t l = 0; l < PEARL_RANK; ++l)
                c->w[((size_t)s * PEARL_RANK + l) * c->n + j] =
                    am.btp[(size_t)j * c->k + (size_t)s * PEARL_RANK + l];
    free(am.ap); free(am.btp); free(am.a16); free(am.bt16);
    c->weights_loaded = 0;
    return 0;
}

static int direct_attempt(pr_qnpu_ctx *c, uint64_t nonce, int legacy_encoding,
                          uint64_t *coords, uint8_t *digests) {
    fused_slot s = {.x=c->slot[0].x};
    const double t0 = now_ms();
    if (legacy_encoding) legacy_nonce_bytes(nonce, s.nonce8);
    else nonce56_bytes(nonce, s.nonce8);
    s.nonce = nonce;
    uint8_t first_chunk[1024];
    memcpy(first_chunk, c->ha, sizeof(first_chunk));
    memcpy(first_chunk, s.nonce8, sizeof(s.nonce8));
    s.commit = c->job_commit;
    pr_htree_patch_chunk(c->tree, c->job_commit.job_key.b, 0, first_chunk);
    pr_htree_root(c->tree, s.commit.root_a.b);
    pr_bind_seeds(s.commit.root_a.b, s.commit.root_b.b, c->m, c->n, &s.commit);
    fill_params(c, &s);
    c->t_noise = now_ms() - t0;
    const double p0 = now_ms();
    int rc = prl_noise_pack_nonce_neon(s.commit.a_noise_seed.b, c->ha,
                                       c->m, s.nonce8, s.x);
    c->t_fold = 0.0;
    c->t_exec = now_ms() - p0;
    if (rc) return rc;
    const double n0 = c->t_exec;
    rc = fused_run(c, &s, FUSED_OUTPUT_MODE_DIGEST, NULL);
    c->t_exec += n0;
    if (rc) return rc;
    fused_result r = {0};
    decode_all_digests(c, &s, &r);
    publish_result(c, &r, coords, digests);
    if (c->debug_ap_sm) {
        for (uint32_t rb = 0; rb < c->m / 64u; ++rb)
            for (uint32_t slice = 0; slice < 16; ++slice)
                for (uint32_t row = 0; row < 64; ++row)
                    for (uint32_t kt = 0; kt < 4; ++kt) {
                        const uint8_t *src = s.x +
                            (((size_t)rb * 16u + slice) * 4u + kt) * 2048u + row * 32u;
                        int8_t *dst = (int8_t *)c->debug_ap_sm +
                            ((size_t)slice * c->m + rb * 64u + row) * PEARL_RANK + kt * 32u;
                        for (unsigned z = 0; z < 32; ++z) dst[z] = (int8_t)(src[z] ^ 0x80u);
                    }
    }
    return r.hits;
}

static void compound_buffers_release(pr_qnpu_ctx *c) {
    if (!c) return;
    for (int i = 0; i < 2; ++i) {
        compound_slot *s = &c->compound[i];
        unmap_persistent(s->x, c->compound_input_bytes);
        unmap_persistent(s->out, s->out_words * sizeof(uint32_t));
        if (s->x) rpcmem_free(s->x);
        if (s->out) rpcmem_free(s->out);
        free(s->params);
        memset(s, 0, sizeof(*s));
        s->state = SLOT_FREE;
    }
    c->compound_available = 0;
    c->prepared_compound_slot = -1;
    c->compound_result_slot = -1;
}

static void compound_backend_init(pr_qnpu_ctx *c) {
    if (c->m != PRL_COMPOUND_ATTEMPT_M || c->k != 2048u) return;
    c->compound_attempts = COMPOUND_ATTEMPTS;
    c->compound_input_bytes = PRL_COMPACT_TOTAL_BYTES(COMPOUND_ATTEMPTS);
    const size_t input_bytes = c->compound_input_bytes;
    const size_t params_words = (size_t)c->compound_attempts * 8u + 8u;
    const size_t result_words = c->bands * c->compound_attempts +
                                PRL_COMPOUND_DIAG_WORDS;
    for (int i = 0; i < 2; ++i) {
        compound_slot *s = &c->compound[i];
        s->out_words = result_words;
        s->x = rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS,
                            (int)input_bytes);
        s->params = calloc(params_words, sizeof(int32_t));
        s->out = rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS,
                              (int)(result_words * sizeof(uint32_t)));
        if (!s->x || !s->params || !s->out ||
            map_persistent(s->x, input_bytes) ||
            map_persistent(s->out, result_words * sizeof(uint32_t))) {
            fprintf(stderr, "[prl-miner] compound buffer allocation/mapping unavailable; using fused single-attempt path\n");
            compound_buffers_release(c);
            return;
        }
    }
    /* Prep pins follow each SoC's topology: 8 Elite (sun) big cores 4-7 by
     * default; override per device with PRL_COMPOUND_PREP_CPUS="a,b,c,d"
     * (8 Gen 3/pineapple: 7=X4 prime, 2-4=A720 3.15G, 5-6=A720 2.96G,
     * 0-1=A520 little). */
    int cpus[4] = {7, 6, 5, 4};
    const char *cpus_env = getenv("PRL_COMPOUND_PREP_CPUS");
    if (cpus_env != NULL) {
        unsigned got = 0;
        for (const char *p = cpus_env; got < 4u && *p;) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p || v < 0 || v >= CPU_SETSIZE) break;
            cpus[got++] = (int)v;
            p = (*end == ',') ? end + 1 : end;
        }
    }
    const unsigned prep_workers = compound_prep_workers();
    const int prep_rc = prl_prep_pool_start(&c->prep_pool, prep_workers, cpus,
                                            prep_pool_pin);
    if (prep_rc != 0) {
        fprintf(stderr, "[prl-miner] compound prep workers unavailable (%d); using fused single-attempt path\n", prep_rc);
        (void)prl_prep_pool_finish(&c->prep_pool);
        compound_buffers_release(c);
        return;
    }
    c->compound[0].state = c->compound[1].state = SLOT_FREE;
    c->prepared_compound_slot = -1;
    c->compound_result_slot = -1;
    c->compound_available = 1;
    fprintf(stderr, "[prl-miner] compound prep pool active: %u workers\n", prep_workers);
}

int pr_qnpu_device_count(char *reason, size_t cap) {
    if (cap) reason[0] = 0;
    if (init_rpc(reason, cap)) return 0;
    if (cap) snprintf(reason, cap, "fused Hexagon HMX FastRPC backend");
    return 1;
}

pr_qnpu_ctx *pr_qnpu_new(uint32_t m, uint32_t n, uint32_t k,
                         char *err, size_t errcap) {
    int expected_bytefold_mode = -1;
    if (errcap) err[0] = 0;
    if (k != 2048 || m < 64 || m > 8192 || m % 64 ||
        n < 64 || n > 2048 || n % 64) {
        if (err && errcap) snprintf(err, errcap,
            "fused backend requires k=2048, m=64..8192 and n=64..2048 (multiples of 64)");
        return NULL;
    }
    if (parse_expected_bytefold_mode(&expected_bytefold_mode)) {
        if (err && errcap) snprintf(err, errcap,
            "PRL_EXPECT_BYTEFOLD_EARLY must be exactly 0 or 1");
        return NULL;
    }
    if (init_rpc(err, errcap)) return NULL;
    pr_qnpu_ctx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->m=m; c->n=n; c->k=k; c->flags=FUSED_FLAGS;
    c->expected_bytefold_mode=expected_bytefold_mode;
    c->alen=(size_t)m*k; c->blen=(size_t)n*k; c->xlen=(size_t)m*k;
    c->bands=(size_t)(m/64u)*(n/64u);
    c->digest_words=c->bands*256u;
    c->out_words=c->digest_words+64u;
    c->prepared_slot=-1;
    c->prepared_compound_slot=-1;
    c->compound_result_slot=-1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    c->base_seed=(uint64_t)ts.tv_sec*1000000007ull ^ (uint64_t)ts.tv_nsec;
    pthread_mutex_init(&c->prep_mu, NULL);
    pthread_cond_init(&c->prep_cv, NULL);

    c->ha=malloc(c->alen); c->hb=malloc(c->blen); c->access_a=malloc(c->alen);
    c->w=rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS, (int)c->blen);
    c->out=rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS,
                         (int)(c->out_words*4u));
    c->slot[0].x=rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS, (int)c->xlen);
    c->slot[1].x=rpcmem_alloc(RPCMEM_DEFAULT_HEAP, RPCMEM_DEFAULT_FLAGS, (int)c->xlen);
    c->debug_ap_sm=malloc(c->alen);
    if (!c->ha || !c->hb || !c->access_a || !c->w || !c->out ||
        !c->slot[0].x || !c->slot[1].x || !c->debug_ap_sm ||
        map_persistent(c->w,c->blen) || map_persistent(c->out,c->out_words*4u) ||
        map_persistent(c->slot[0].x,c->xlen) || map_persistent(c->slot[1].x,c->xlen)) {
        if (err && errcap) snprintf(err, errcap, "fused backend buffer allocation/mapping failed");
        pr_qnpu_free(c);
        return NULL;
    }
    c->slot[0].state=c->slot[1].state=SLOT_FREE;
    compound_backend_init(c);
    if (pthread_create(&c->prep_thread, NULL, prep_worker, c) != 0) {
        if (err && errcap) snprintf(err, errcap, "fused input worker creation failed");
        pr_qnpu_free(c);
        return NULL;
    }
    c->prep_thread_started=1;
    return c;
}

void pr_qnpu_free(pr_qnpu_ctx *c) {
    if (!c) return;
    if (c->prep_thread_started) {
        pthread_mutex_lock(&c->prep_mu);
        c->prep_quit=1;
        pthread_cond_broadcast(&c->prep_cv);
        pthread_mutex_unlock(&c->prep_mu);
        pthread_join(c->prep_thread, NULL);
    }
    (void)prl_prep_pool_finish(&c->prep_pool);
    if (c->tree) pr_htree_free(c->tree);
    unmap_persistent(c->slot[0].x,c->xlen);
    unmap_persistent(c->slot[1].x,c->xlen);
    unmap_persistent(c->w,c->blen);
    unmap_persistent(c->out,c->out_words*4u);
    for (int i = 0; i < 2; ++i) {
        unmap_persistent(c->compound[i].x, c->compound_input_bytes);
        unmap_persistent(c->compound[i].out,
                         c->compound[i].out_words * sizeof(uint32_t));
        if (c->compound[i].x) rpcmem_free(c->compound[i].x);
        if (c->compound[i].out) rpcmem_free(c->compound[i].out);
        free(c->compound[i].params);
    }
    if (c->slot[0].x) rpcmem_free(c->slot[0].x);
    if (c->slot[1].x) rpcmem_free(c->slot[1].x);
    if (c->w) rpcmem_free(c->w);
    if (c->out) rpcmem_free(c->out);
    free(c->ha); free(c->hb); free(c->access_a); free(c->debug_ap_sm);
    pthread_cond_destroy(&c->prep_cv);
    pthread_mutex_destroy(&c->prep_mu);
    free(c);
}

void pr_qnpu_set_base_seed(pr_qnpu_ctx *c, uint64_t seed) {
    if (c) c->base_seed=seed;
}

int pr_qnpu_set_job(pr_qnpu_ctx *c, const uint8_t header76[76],
                    char *err, size_t errcap) {
    if (!c || !header76) return -1;
    if (errcap) err[0]=0;
    pthread_mutex_lock(&c->prep_mu);
    while (c->prep_busy) pthread_cond_wait(&c->prep_cv, &c->prep_mu);
    c->prep_task_kind=0;
    c->slot[0].state=c->slot[1].state=SLOT_FREE;
    c->prepared_slot=-1;
    c->compound[0].state=c->compound[1].state=SLOT_FREE;
    c->prepared_compound_slot=-1;
    c->compound_result_slot=-1;
    pthread_mutex_unlock(&c->prep_mu);
    c->pending.valid=0;

    pr_attempt_mats base={.m=c->m,.n=c->n,.k=c->k,.a=c->ha,.bt=c->hb};
    pr_fill_base(&base,c->base_seed);
    /* Each mapped slot owns the immutable base for this job. The worker was
     * drained above; individual nonce records follow the copied base. */
    if (c->compound_available) {
        for (int i = 0; i < 2; ++i)
            memcpy(c->compound[i].x, c->ha, PRL_COMPACT_BASE_BYTES);
    }
    memcpy(c->access_a,c->ha,c->alen);
    pr_commit(header76,c->k,c->ha,c->hb,c->m,c->n,&c->job_commit);
    pr_htree *tree=pr_htree_new(c->job_commit.job_key.b,(const uint8_t *)c->ha,c->alen);
    if (!tree) {
        if (err && errcap) snprintf(err,errcap,"fused job Merkle tree allocation failed");
        return -1;
    }
    if (c->tree) pr_htree_free(c->tree);
    c->tree=tree;
    if (setup_job_weights(c,err,errcap)) return -1;
    c->compound_weights_loaded=0;
    return 0;
}

void pr_qnpu_set_bound(pr_qnpu_ctx *c, const uint8_t bound_le[32]) {
    if (c && bound_le) memcpy(&c->bound,bound_le,32);
}

int pr_qnpu_attempt(pr_qnpu_ctx *c, uint64_t nonce,
                    uint64_t *coords, uint8_t *digests) {
    if (!c || !c->tree) return -1;
    const int hits=direct_attempt(c,nonce,1,coords,digests);
    if (hits < 0) return hits;
    c->macs+=(uint64_t)c->m*c->n*c->k;
    return hits;
}

int pr_qnpu_attempt_pipe(pr_qnpu_ctx *c, uint64_t nonce,
                         uint64_t *coords, uint8_t *digests) {
    return pr_qnpu_attempt_pipe2(c,nonce,nonce,coords,digests);
}

static int return_pending(pr_qnpu_ctx *c, uint64_t *coords, uint8_t *digests) {
    if (!c->pending.valid) return 0;
    fused_result r=c->pending;
    c->pending.valid=0;
    publish_result(c,&r,coords,digests);
    return r.hits;
}

int pr_qnpu_attempt_pipe2(pr_qnpu_ctx *c, uint64_t nonce, uint64_t next_nonce,
                          uint64_t *coords, uint8_t *digests) {
    if (!c || !c->tree) return -1;
    static int main_pinned;
    if (!main_pinned) { pin_current_thread(6); main_pinned=1; }
    const int old_hits=return_pending(c,coords,digests);
    int cur=c->prepared_slot;
    if (cur<0) {
        cur=0;
        if (schedule_prep(c,cur,nonce) || wait_slot_ready(c,cur)) {
            c->fused_failed=1;
            fprintf(stderr,"[prl-miner] fused input prime failed\n");
            return old_hits;
        }
    } else if (wait_slot_ready(c,cur) || c->slot[cur].nonce!=nonce) {
        fprintf(stderr,"[prl-miner] fused input nonce pipeline mismatch; rebuilding nonce %llu\n",
                (unsigned long long)nonce);
        slot_free(c,cur);
        if (schedule_prep(c,cur,nonce) || wait_slot_ready(c,cur)) {
            c->fused_failed=1;
            return old_hits;
        }
    }
    c->prepared_slot=-1;
    c->slot[cur].state=SLOT_EXECUTING;

    const int next=1-cur;
    if (next_nonce!=nonce && schedule_prep(c,next,next_nonce)==0)
        c->prepared_slot=next;

    int rc=fused_run(c,&c->slot[cur],FUSED_OUTPUT_MODE_MASK,NULL);
    if (rc==0) {
        fused_result r={0};
        r.valid=1; r.nonce=c->slot[cur].nonce;
        r.commit=c->slot[cur].commit;
        memcpy(r.nonce8,c->slot[cur].nonce8,sizeof(r.nonce8));
        decode_masks(c,&r);
        c->pending=r;
        c->macs+=(uint64_t)c->m*c->n*c->k;
        c->t_noise=c->slot[cur].seed_ms+c->slot[cur].pack_ms;
        c->t_fold=0.0;
    } else {
        c->fused_failed=1;
        memset(&c->pending,0,sizeof(c->pending));
    }
    slot_free(c,cur);
    return old_hits;
}

int pr_qnpu_drain(pr_qnpu_ctx *c, uint64_t *coords, uint8_t *digests) {
    if (!c) return 0;
    pthread_mutex_lock(&c->prep_mu);
    while (c->prep_busy) pthread_cond_wait(&c->prep_cv,&c->prep_mu);
    if (c->prepared_slot>=0) c->slot[c->prepared_slot].state=SLOT_FREE;
    c->prepared_slot=-1;
    if (c->prepared_compound_slot>=0)
        c->compound[c->prepared_compound_slot].state=SLOT_FREE;
    if (c->compound_result_slot>=0)
        c->compound[c->compound_result_slot].state=SLOT_FREE;
    c->prepared_compound_slot=-1;
    c->compound_result_slot=-1;
    pthread_mutex_unlock(&c->prep_mu);
    return return_pending(c,coords,digests);
}

const int8_t *pr_qnpu_host_a(pr_qnpu_ctx *c) { return c?c->access_a:NULL; }
const int8_t *pr_qnpu_host_bt(pr_qnpu_ctx *c) { return c?c->hb:NULL; }
const pr_commitment *pr_qnpu_commitment(pr_qnpu_ctx *c) { return c?&c->access_commit:NULL; }
uint64_t pr_qnpu_macs(pr_qnpu_ctx *c) { return c?c->macs:0; }

void pr_qnpu_debug_read_ap(pr_qnpu_ctx *c, void *dst, size_t n) {
    if (!c || !dst || !c->debug_ap_sm) return;
    memcpy(dst,c->debug_ap_sm,n<c->alen?n:c->alen);
}

void pr_qnpu_debug_acc(pr_qnpu_ctx *c, uint32_t tr, uint32_t tc,
                       int32_t out[16][8][16]) {
    if (!c || !out) return;
    pr_attempt_mats am={.m=c->m,.n=c->n,.k=c->k,.a=c->access_a,.bt=c->hb};
    am.ap=malloc(c->alen); am.btp=malloc(c->blen);
    am.a16=malloc(c->alen*sizeof(int16_t)); am.bt16=malloc(c->blen*sizeof(int16_t));
    if (!am.ap||!am.btp||!am.a16||!am.bt16) goto done;
    pr_noise_operands(&am,&c->access_commit);
    for (unsigned s=0;s<16;s++) for (unsigned u=0;u<8;u++) for (unsigned v=0;v<16;v++) {
        const int8_t *a=am.ap+(size_t)(tr+pr_row_off(u))*c->k+(size_t)s*PEARL_RANK;
        const int8_t *b=am.btp+(size_t)(tc+pr_col_off(v))*c->k+(size_t)s*PEARL_RANK;
        int32_t sum=0; for (unsigned l=0;l<PEARL_RANK;l++) sum+=(int32_t)a[l]*(int32_t)b[l];
        out[s][u][v]=sum;
    }
done:
    free(am.ap); free(am.btp); free(am.a16); free(am.bt16);
}

void pr_qnpu_last_timing(pr_qnpu_ctx *c, double *noise_ms,
                         double *exec_ms, double *fold_ms) {
    if (!c) return;
    if (noise_ms) *noise_ms=c->t_noise;
    if (exec_ms) *exec_ms=c->t_exec;
    if (fold_ms) *fold_ms=c->t_fold;
}
