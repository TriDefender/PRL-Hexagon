/* Band-batched SPSC transport, derived from the verified multi transport.
 * Each worker owns one 256 KiB VTCM buffer containing [half2][slice16][8KiB].
 * Producer acquires it once, computes all 32 retained snapshots, completes
 * HMX stores with SYNCHT and publishes once. Consumer processes the entire
 * band, completes its reads/stores and releases once. No slot is reused
 * while its previous band is in flight. FIFO ownership is band % workers.
 *
 * Counts, processed and produced are BANDS here, not 64x32 snapshot tiles.
 * The callback receives (context, row_block, band, unused, raw_band_base).
 * Cache layout can reuse PRL_MULTI_RAW_BYTES(workers): the byte count is equal.
 */
#ifndef PRL_FUSED_BAND_H
#define PRL_FUSED_BAND_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "AEEStdErr.h"
#include "HAP_perf.h"
#include "qurt_thread.h"
#include "qurt_hvx.h"

#define PRL_BAND_MAX_WORKERS 4u
#define PRL_BAND_SLOTS 1u
#define PRL_BAND_SLOT_BYTES (32u*8192u)
#define PRL_BAND_STACK_BYTES 32768u
#define PRL_BAND_RAW_BYTES(workers) ((uint32_t)(workers)*PRL_BAND_SLOTS*PRL_BAND_SLOT_BYTES)

typedef void (*prl_band_consume_fn)(void *,int,int,int,const uint8_t *);
typedef struct __attribute__((aligned(64))) {
    uint32_t seq;
    int rb,cb,s;
    uint8_t pad[48];
} prl_band_slot;
typedef struct {
    prl_band_slot slot[PRL_BAND_SLOTS];
    uint32_t stop,started,processed,quiet;
    int status,profile,lock_rc,unlock_rc,join_rc,join_status;
    uint32_t count,produced,raw_offset;
    uint8_t *vtcm;
    void *context;
    prl_band_consume_fn consume;
    qurt_thread_t tid;
    void *stack_raw;
    int created;
    uint64_t busy_cycles,producer_wait_cycles;
} prl_band_worker;
typedef struct __attribute__((aligned(64))) {
    prl_band_worker worker[PRL_BAND_MAX_WORKERS];
    unsigned workers;
    void *allocation;
} prl_band;

static prl_band *prl_band_alloc(void) {
    void *allocation=malloc(sizeof(prl_band)+63u);
    if (!allocation) return NULL;
    prl_band *p=(prl_band *)(((uintptr_t)allocation+63u)&~(uintptr_t)63u);
    memset(p,0,sizeof(*p));p->allocation=allocation;
    return p;
}
static void prl_band_free(prl_band *p) {
    if (!p) return;
    for (unsigned k=0;k<p->workers;k++)
        if (p->worker[k].created) return; /* failed join: metadata is still live */
    free(p->allocation);
}
static inline void prl_band_complete(void) {
#ifdef __hexagon__
    __asm__ volatile("syncht" ::: "memory");
#else
    /* Host ownership-protocol tests do not model asynchronous HMX writes. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline int prl_band_stopped(prl_band_worker *w) {
    return __atomic_load_n(&w->stop,__ATOMIC_ACQUIRE)!=0;
}
static int prl_band_wait(prl_band_worker *w,uint32_t *word,uint32_t expected) {
    unsigned spins=0;
    const uint64_t start=HAP_perf_get_time_us();
    while (__atomic_load_n(word,__ATOMIC_ACQUIRE)!=expected) {
        if (prl_band_stopped(w)) return AEE_EFAILED;
        if ((++spins&4095u)==0) {
            if (HAP_perf_get_time_us()-start>2000000u) {
                int empty=0;
                __atomic_compare_exchange_n(&w->status,&empty,AEE_EEXPIRED,0,
                                            __ATOMIC_RELEASE,__ATOMIC_RELAXED);
                __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
                return AEE_EEXPIRED;
            }
            qurt_sleep(1);
        }
    }
    return 0;
}
static void prl_band_worker_main(void *arg) {
    prl_band_worker *w=(prl_band_worker *)arg;
    /* Request a virtualized 128-byte HVX context. JOINABLE below preserves
     * the exited worker until its parent collects the status. */
    const int lock=qurt_hvx_lock(QURT_HVX_MODE_128B);
    __atomic_store_n(&w->lock_rc,lock,__ATOMIC_RELEASE);
    if (lock) {
        int empty=0;
        __atomic_compare_exchange_n(&w->status,&empty,AEE_EFAILED,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&w->started,1,__ATOMIC_RELEASE);
    if (lock) {
        __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
        __atomic_store_n(&w->quiet,1,__ATOMIC_RELEASE);
        qurt_thread_exit(lock);
        return;
    }
    for (uint32_t i=0;i<w->count;i++) {
        prl_band_slot *slot=&w->slot[i%PRL_BAND_SLOTS];
        if (prl_band_wait(w,&slot->seq,2u*i+1u)) break;
        const uint64_t t=w->profile?HAP_perf_get_pcycles():0;
        w->consume(w->context,slot->rb,slot->cb,slot->s,
            w->vtcm+w->raw_offset+(i%PRL_BAND_SLOTS)*PRL_BAND_SLOT_BYTES);
        prl_band_complete();
        if (w->profile) w->busy_cycles+=HAP_perf_get_pcycles()-t;
        __atomic_store_n(&slot->seq,2u*(i+PRL_BAND_SLOTS),__ATOMIC_RELEASE);
        __atomic_store_n(&w->processed,i+1u,__ATOMIC_RELEASE);
    }
    /* Finish may return despite a failed join only after all callback RPC
     * output/context accesses and their memory transactions have completed. */
    prl_band_complete();
    __atomic_store_n(&w->quiet,1,__ATOMIC_RELEASE);
    const int unlock=qurt_hvx_unlock();
    __atomic_store_n(&w->unlock_rc,unlock,__ATOMIC_RELEASE);
    if (unlock) {
        int empty=0;
        __atomic_compare_exchange_n(&w->status,&empty,AEE_EFAILED,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    qurt_thread_exit(__atomic_load_n(&w->status,__ATOMIC_ACQUIRE));
}
static int prl_band_finish(prl_band *p,int abort) {
    if (abort) for (unsigned k=0;k<p->workers;k++)
        __atomic_store_n(&p->worker[k].stop,1,__ATOMIC_RELEASE);
    int first_error=0;
    for (unsigned k=0;k<p->workers;k++) {
        prl_band_worker *w=&p->worker[k];
        if (!w->created) continue;
        int status=0;
        const int rc=qurt_thread_join(w->tid,&status);
        w->join_rc=rc;w->join_status=status;
        /* Join normally follows quiet with no additional timer wakeup. A
         * failed join does not retain the caller's RPC mapping, so wait for
         * every callback access plus SYNCHT without an unsafe timeout. */
        while (!__atomic_load_n(&w->quiet,__ATOMIC_ACQUIRE)) qurt_sleep(1);
        if (rc) {
            /* A failed join does not prove termination; keep the worker stack
             * allocated. The caller must not reuse VTCM after such a failure. */
            if (!first_error) first_error=AEE_EFAILED;
            continue;
        }
        w->created=0;free(w->stack_raw);w->stack_raw=NULL;
        int error=__atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
        if (!error&&status) error=status;
        if (!error && !abort && __atomic_load_n(&w->processed,__ATOMIC_ACQUIRE)!=w->count)
            error=AEE_EFAILED;
        if (!first_error && error) first_error=error;
    }
    return first_error;
}
static int prl_band_start(prl_band *p,uint8_t *vtcm,uint32_t raw_offset,
                           int m,int n,unsigned workers,int profile,
                           prl_band_consume_fn consume,void *context) {
    if (!p||!vtcm||!consume||m<64||n<64||m%64||n%64||
        workers<1||workers>PRL_BAND_MAX_WORKERS||raw_offset%2048u)
        return AEE_EBADPARM;
    void *allocation=p->allocation;
    memset(p,0,sizeof(*p));p->allocation=allocation;
    const unsigned bands=(unsigned)n/64u;
    if (workers>bands) workers=bands;
    p->workers=workers;
    int error=0;
    for (unsigned k=0;k<workers;k++) {
        prl_band_worker *w=&p->worker[k];
        for (unsigned i=0;i<PRL_BAND_SLOTS;i++) w->slot[i].seq=2u*i;
        const uint32_t owned=(bands+workers-1u-k)/workers;
        w->count=(uint32_t)(m/64)*owned;
        w->vtcm=vtcm;w->raw_offset=raw_offset+k*PRL_BAND_SLOTS*PRL_BAND_SLOT_BYTES;
        w->profile=profile;w->context=context;w->consume=consume;
        w->stack_raw=malloc(PRL_BAND_STACK_BYTES+127u);
        if (!w->stack_raw) {error=AEE_ENOMEMORY;break;}
        void *stack=(void *)(((uintptr_t)w->stack_raw+127u)&~(uintptr_t)127u);
        qurt_thread_attr_t attr;qurt_thread_attr_init(&attr);
        /* LEGACY threads disappear as soon as they exit; a completed worker
         * must remain joinable while the producer reaps its peer workers. */
        qurt_thread_attr_set_detachstate(&attr,QURT_THREAD_ATTR_CREATE_JOINABLE);
        const char *names[4]={"prl_hvx_band0","prl_hvx_band1","prl_hvx_band2","prl_hvx_band3"};
        qurt_thread_attr_set_name(&attr,names[k]);
        qurt_thread_attr_set_stack_addr(&attr,stack);
        qurt_thread_attr_set_stack_size(&attr,PRL_BAND_STACK_BYTES);
        const int priority=qurt_thread_get_priority(qurt_thread_get_id());
        if (priority>=0&&priority<=255)
            qurt_thread_attr_set_priority(&attr,(unsigned short)priority);
        const int rc=qurt_thread_create(&w->tid,&attr,prl_band_worker_main,w);
        if (rc) {free(w->stack_raw);w->stack_raw=NULL;error=AEE_EFAILED;break;}
        w->created=1;
        error=prl_band_wait(w,&w->started,1u);
        if (!error) error=__atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
        if (error) break;
    }
    if (error) {prl_band_finish(p,1);return error;}
    return 0;
}
static int prl_band_acquire(prl_band *p,int band,unsigned *worker,
                             uint32_t *tag,uint32_t *raw_offset) {
    if (band<0||!p->workers) return AEE_EBADPARM;
    const unsigned owner=(unsigned)band%p->workers;
    prl_band_worker *w=&p->worker[owner];
    const uint32_t i=w->produced;
    if (i>=w->count) return AEE_EBADSIZE;
    const uint64_t t=w->profile?HAP_perf_get_pcycles():0;
    const int rc=prl_band_wait(w,&w->slot[i%PRL_BAND_SLOTS].seq,2u*i);
    if (w->profile) w->producer_wait_cycles+=HAP_perf_get_pcycles()-t;
    if (!rc) {
        *worker=owner;*tag=i;
        *raw_offset=w->raw_offset+(i%PRL_BAND_SLOTS)*PRL_BAND_SLOT_BYTES;
    }
    return rc;
}
static void prl_band_publish(prl_band *p,unsigned worker,uint32_t tag,
                              int rb,int cb,int s) {
    prl_band_worker *w=&p->worker[worker];
    prl_band_slot *slot=&w->slot[tag%PRL_BAND_SLOTS];
    slot->rb=rb;slot->cb=cb;slot->s=s;
    prl_band_complete();
    __atomic_store_n(&slot->seq,2u*tag+1u,__ATOMIC_RELEASE);
    w->produced=tag+1u;
}
#endif
