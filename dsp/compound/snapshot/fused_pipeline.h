/* SPSC transport for VTCM HMX exports and a dedicated HVX consumer.
 *
 * Ownership: producer owns HMX and each free raw slot, consumer owns g_cum and
 * output. Metadata/sequence counters live in coherent ordinary DSP memory;
 * 8 KiB raw slot payloads live in VTCM. A slot is not reused until the consumer
 * has completed its HVX reads. The producer completes HMX writes before publish.
 *
 * SDK qurt_memory.h documents SYNCHT as completing all previous memory
 * transactions from the issuing thread and making them globally observable.
 * Both ownership transfers use this conservative completion plus C atomics.
 * This is deliberately stronger than a compiler barrier. HMX-specific cross-
 * thread visibility still needs repeated full device comparisons against CPU.
 */
#ifndef PRL_FUSED_PIPELINE_H
#define PRL_FUSED_PIPELINE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "qurt_thread.h"
#include "qurt_hvx.h"

#define PRL_PIPE_SLOTS 4u
#define PRL_PIPE_SLOT_BYTES 8192u
#define PRL_PIPE_STACK_BYTES 32768u

typedef void (*prl_pipe_consume_fn)(void *,int,int,int,const uint8_t *);

typedef struct __attribute__((aligned(64))) {
    uint32_t seq;
    int rb,cb,s;
    uint8_t pad[48];
} prl_pipe_slot;

typedef struct {
    prl_pipe_slot slot[PRL_PIPE_SLOTS];
    uint32_t stop,started,processed,quiet;
    int status,profile,created,lock_rc,unlock_rc,join_rc,join_status;
    uint32_t count,raw_offset;
    uint8_t *vtcm;
    void *context;
    prl_pipe_consume_fn consume;
    qurt_thread_t tid;
    void *stack_raw;
    uint64_t busy_cycles,producer_wait_cycles;
} prl_pipe;

static inline void prl_pipe_complete(void) {
#ifdef __hexagon__
    __asm__ volatile("syncht" ::: "memory");
#else
    /* Host tests exercise ownership/lifetime, not asynchronous HMX writes. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline int prl_pipe_stopped(prl_pipe *p) {
    return __atomic_load_n(&p->stop,__ATOMIC_ACQUIRE)!=0;
}
static int prl_pipe_wait(prl_pipe *p,uint32_t *word,uint32_t expected) {
    unsigned spins=0;
    uint64_t start=HAP_perf_get_time_us();
    while (__atomic_load_n(word,__ATOMIC_ACQUIRE)!=expected) {
        if (prl_pipe_stopped(p)) return AEE_EFAILED;
        if ((++spins&4095u)==0) {
            /* Avoid an unbounded join if the peer could not run. This deadline
             * is per ownership transfer, far longer than a normal tile. */
            if (HAP_perf_get_time_us()-start>2000000u) {
                int empty=0;
                __atomic_compare_exchange_n(&p->status,&empty,AEE_EEXPIRED,0,
                                            __ATOMIC_RELEASE,__ATOMIC_RELAXED);
                __atomic_store_n(&p->stop,1,__ATOMIC_RELEASE);
                return AEE_EEXPIRED;
            }
            qurt_sleep(1);
        }
    }
    return 0;
}
static void prl_pipe_worker(void *arg) {
    prl_pipe *p=(prl_pipe *)arg;
    /* V65+ maps normal lock to a context-switchable HVX set request. The
     * immediate-use try_lock can fail when physical units are transiently busy
     * even though normal context switching can execute this worker. */
    int rc=qurt_hvx_lock(QURT_HVX_MODE_128B);
    __atomic_store_n(&p->lock_rc,rc,__ATOMIC_RELEASE);
    if (rc) {
        int empty=0;
        __atomic_compare_exchange_n(&p->status,&empty,AEE_EFAILED,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&p->started,1,__ATOMIC_RELEASE);
    if (rc) {
        __atomic_store_n(&p->stop,1,__ATOMIC_RELEASE);
        __atomic_store_n(&p->quiet,1,__ATOMIC_RELEASE);
        qurt_thread_exit(rc);
        return;
    }
    for (uint32_t i=0;i<p->count;i++) {
        prl_pipe_slot *slot=&p->slot[i%PRL_PIPE_SLOTS];
        if (prl_pipe_wait(p,&slot->seq,i+1u)) break;
        const uint64_t t=p->profile?HAP_perf_get_pcycles():0;
        p->consume(p->context,slot->rb,slot->cb,slot->s,
                   p->vtcm+p->raw_offset+(i%PRL_PIPE_SLOTS)*PRL_PIPE_SLOT_BYTES);
        prl_pipe_complete();
        if (p->profile) p->busy_cycles+=HAP_perf_get_pcycles()-t;
        __atomic_store_n(&slot->seq,i+PRL_PIPE_SLOTS,__ATOMIC_RELEASE);
        __atomic_store_n(&p->processed,i+1u,__ATOMIC_RELEASE);
    }
    /* No callback or external RPC output/context access follows quiet. */
    prl_pipe_complete();
    __atomic_store_n(&p->quiet,1,__ATOMIC_RELEASE);
    const int unlock=qurt_hvx_unlock();
    __atomic_store_n(&p->unlock_rc,unlock,__ATOMIC_RELEASE);
    if (unlock) {
        int empty=0;
        __atomic_compare_exchange_n(&p->status,&empty,AEE_EFAILED,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    qurt_thread_exit(__atomic_load_n(&p->status,__ATOMIC_ACQUIRE));
}
static int prl_pipe_finish(prl_pipe *p,int abort);
static int prl_pipe_start(prl_pipe *p,uint8_t *vtcm,uint32_t raw_offset,
                          uint32_t count,int profile,
                          prl_pipe_consume_fn consume,void *context) {
    memset(p,0,sizeof(*p));
    for (unsigned i=0;i<PRL_PIPE_SLOTS;i++) p->slot[i].seq=i;
    p->vtcm=vtcm;p->raw_offset=raw_offset;p->count=count;
    p->profile=profile;p->consume=consume;p->context=context;
    p->stack_raw=malloc(PRL_PIPE_STACK_BYTES+127u);
    if (!p->stack_raw) return AEE_ENOMEMORY;
    void *stack=(void *)(((uintptr_t)p->stack_raw+127u)&~(uintptr_t)127u);
    qurt_thread_attr_t attr;
    qurt_thread_attr_init(&attr);
    /* The QuRT default is LEGACY, which discards an exited thread before a
     * later join. Explicit JOINABLE preserves termination until it is reaped. */
    qurt_thread_attr_set_detachstate(&attr,QURT_THREAD_ATTR_CREATE_JOINABLE);
    qurt_thread_attr_set_name(&attr,"prl_hvx_fold");
    qurt_thread_attr_set_stack_addr(&attr,stack);
    qurt_thread_attr_set_stack_size(&attr,PRL_PIPE_STACK_BYTES);
    int priority=qurt_thread_get_priority(qurt_thread_get_id());
    if (priority>=0 && priority<=255) qurt_thread_attr_set_priority(&attr,(unsigned short)priority);
    int rc=qurt_thread_create(&p->tid,&attr,prl_pipe_worker,p);
    if (rc) { free(p->stack_raw);p->stack_raw=NULL;return AEE_EFAILED; }
    p->created=1;
    rc=prl_pipe_wait(p,&p->started,1);
    if (rc || __atomic_load_n(&p->status,__ATOMIC_ACQUIRE)) {
        const int first_error=rc?rc:__atomic_load_n(&p->status,__ATOMIC_ACQUIRE);
        const int finish_error=prl_pipe_finish(p,1);
        return first_error?first_error:finish_error;
    }
    return 0;
}
static int prl_pipe_acquire(prl_pipe *p,uint32_t i,uint32_t *raw_offset) {
    uint64_t t=p->profile?HAP_perf_get_pcycles():0;
    int rc=prl_pipe_wait(p,&p->slot[i%PRL_PIPE_SLOTS].seq,i);
    if (p->profile) p->producer_wait_cycles+=HAP_perf_get_pcycles()-t;
    if (!rc) *raw_offset=p->raw_offset+(i%PRL_PIPE_SLOTS)*PRL_PIPE_SLOT_BYTES;
    return rc;
}
static void prl_pipe_publish(prl_pipe *p,uint32_t i,int rb,int cb,int s) {
    prl_pipe_slot *slot=&p->slot[i%PRL_PIPE_SLOTS];
    slot->rb=rb;slot->cb=cb;slot->s=s;
    prl_pipe_complete();
    __atomic_store_n(&slot->seq,i+1u,__ATOMIC_RELEASE);
}
static int prl_pipe_finish(prl_pipe *p,int abort) {
    if (!p) return 0;
    if (!p->created) return __atomic_load_n(&p->status,__ATOMIC_ACQUIRE);
    if (abort) __atomic_store_n(&p->stop,1,__ATOMIC_RELEASE);
    int status=0;
    int rc=qurt_thread_join(p->tid,&status);
    p->join_rc=rc;p->join_status=status;
    /* Successful join already follows quiet, avoiding an extra timer wait.
     * On join failure, never time out and let an external RPC writer outlive
     * the mapping: all callback accesses plus SYNCHT must complete first. */
    while (!__atomic_load_n(&p->quiet,__ATOMIC_ACQUIRE)) qurt_sleep(1);
    if (rc) return AEE_EFAILED;
    p->created=0;free(p->stack_raw);p->stack_raw=NULL;
    int error=__atomic_load_n(&p->status,__ATOMIC_ACQUIRE);
    if (!error&&status) error=status;
    if (error) return error;
    if (!abort && __atomic_load_n(&p->processed,__ATOMIC_ACQUIRE)!=p->count) return AEE_EFAILED;
    return 0;
}

#endif
