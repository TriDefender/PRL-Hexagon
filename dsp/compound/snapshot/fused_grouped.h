/* Optional production transport: exact retained-prefix groups, fixed 256 KiB/worker.
 * All snapshots in a group must be fully exported before publish. Producer
 * SYNCHT+release publishes the whole group; the consumer invokes the unchanged
 * callback in increasing s order, then SYNCHT+release returns the whole slot.
 * This protocol model does NOT prove pending HMX exports survive later MMs.
 */
#ifndef PRL_FUSED_GROUPED_H
#define PRL_FUSED_GROUPED_H
#include "fused_multi.h"
#ifndef PRL_SNAPSHOT_GROUP
#define PRL_SNAPSHOT_GROUP 1
#endif
#if PRL_SNAPSHOT_GROUP != 1 && PRL_SNAPSHOT_GROUP != 2 && PRL_SNAPSHOT_GROUP != 4 && PRL_SNAPSHOT_GROUP != 8 && PRL_SNAPSHOT_GROUP != 16
#error PRL_SNAPSHOT_GROUP must be 1, 2, 4, 8 or 16
#endif
#ifndef PRL_GROUP_WORKERS
#define PRL_GROUP_WORKERS 0
#endif
#if PRL_GROUP_WORKERS != 0 && PRL_GROUP_WORKERS != 2 && PRL_GROUP_WORKERS != 3 && PRL_GROUP_WORKERS != 4
#error PRL_GROUP_WORKERS must be 0, 2, 3 or 4
#endif
/* Runtime bits remain unchanged: only these exact hot calls accept an override. */
static unsigned requested_multi_workers(int flags) {
#if PRL_GROUP_WORKERS
    if (flags==2930||flags==35698) return PRL_GROUP_WORKERS;
#endif
    return (flags&1536)==1536?3u:(flags&1024)?4u:(flags&512)?2u:0u;
}
#if PRL_SNAPSHOT_GROUP > 1 && PRL_SPECIALIZE_HOT && PRL_SPECIALIZE_QUEUE
#define PRL_GROUPED_ENABLED 1
#else
#define PRL_GROUPED_ENABLED 0
#endif
#define PRL_GROUPED_LOG2 (PRL_SNAPSHOT_GROUP==16?4u:PRL_SNAPSHOT_GROUP==8?3u:PRL_SNAPSHOT_GROUP==4?2u:PRL_SNAPSHOT_GROUP==2?1u:0u)
#define PRL_GROUPED_DIAG_BITS (PRL_GROUPED_LOG2<<4)
static inline int prl_grouped_enabled(int flags,int known_queue) {
    return PRL_GROUPED_ENABLED&&known_queue&&(flags==2930||flags==35698);
}
#define PRL_GROUPED_SLOTS (32u/PRL_SNAPSHOT_GROUP)
#define PRL_GROUPED_SLOT_BYTES (8192u*PRL_SNAPSHOT_GROUP)
static void prl_grouped_worker_main(void *arg) {
    prl_multi_worker *w=(prl_multi_worker *)arg;
    /* Request a virtualized 128-byte HVX context. The historical intermittent
     * exit failure was caused by LEGACY joins, not a proven lock failure. */
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
        prl_multi_slot *slot=&w->slot[i%PRL_GROUPED_SLOTS];
        if (prl_multi_wait(w,&slot->seq,i+1u)) break;
        const uint64_t t=w->profile?HAP_perf_get_pcycles():0;
        const uint8_t *raw=w->vtcm+w->raw_offset+(i%PRL_GROUPED_SLOTS)*PRL_GROUPED_SLOT_BYTES;
        for (unsigned j=0;j<PRL_SNAPSHOT_GROUP;j++)
            w->consume(w->context,slot->rb,slot->cb,slot->s+(int)j,
                        raw+j*PRL_MULTI_SLOT_BYTES);
        prl_multi_complete();
        if (w->profile) w->busy_cycles+=HAP_perf_get_pcycles()-t;
        __atomic_store_n(&slot->seq,i+PRL_GROUPED_SLOTS,__ATOMIC_RELEASE);
        __atomic_store_n(&w->processed,i+1u,__ATOMIC_RELEASE);
    }
    /* A failed join may leave this thread alive. Prove that all callback
     * accesses to the caller's RPC output/context are complete before finish
     * is allowed to return; retaining metadata alone cannot retain RPC memory. */
    prl_multi_complete();
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
static int prl_grouped_start(prl_multi *p,uint8_t *vtcm,uint32_t raw_offset,
                           int m,int n,unsigned workers,int profile,
                           prl_multi_consume_fn consume,void *context) {
    if (!p||!vtcm||!consume||m<64||n<64||m%64||n%64||
        workers<1||workers>PRL_MULTI_MAX_WORKERS||raw_offset%2048u)
        return AEE_EBADPARM;
    void *allocation=p->allocation;
    memset(p,0,sizeof(*p));p->allocation=allocation;
    const unsigned bands=(unsigned)n/64u;
    if (workers>bands) workers=bands;
    p->workers=workers;
    int error=0;
    for (unsigned k=0;k<workers;k++) {
        prl_multi_worker *w=&p->worker[k];
        for (unsigned i=0;i<PRL_GROUPED_SLOTS;i++) w->slot[i].seq=i;
        const uint32_t owned=(bands+workers-1u-k)/workers;
        w->count=(uint32_t)(m/64)*owned*(32u/PRL_SNAPSHOT_GROUP);
        w->vtcm=vtcm;w->raw_offset=raw_offset+k*PRL_GROUPED_SLOTS*PRL_GROUPED_SLOT_BYTES;
        w->profile=profile;w->context=context;w->consume=consume;
        w->stack_raw=malloc(PRL_MULTI_STACK_BYTES+127u);
        if (!w->stack_raw) {error=AEE_ENOMEMORY;break;}
        void *stack=(void *)(((uintptr_t)w->stack_raw+127u)&~(uintptr_t)127u);
        qurt_thread_attr_t attr;qurt_thread_attr_init(&attr);
        /* LEGACY threads disappear as soon as they exit; a completed worker
         * must remain joinable while the producer reaps its peer workers. */
        qurt_thread_attr_set_detachstate(&attr,QURT_THREAD_ATTR_CREATE_JOINABLE);
        const char *names[4]={"prl_group_fold0","prl_group_fold1","prl_group_fold2","prl_group_fold3"};
        qurt_thread_attr_set_name(&attr,names[k]);
        qurt_thread_attr_set_stack_addr(&attr,stack);
        qurt_thread_attr_set_stack_size(&attr,PRL_MULTI_STACK_BYTES);
        const int priority=qurt_thread_get_priority(qurt_thread_get_id());
        if (priority>=0&&priority<=255)
            qurt_thread_attr_set_priority(&attr,(unsigned short)priority);
        const int rc=qurt_thread_create(&w->tid,&attr,prl_grouped_worker_main,w);
        if (rc) {free(w->stack_raw);w->stack_raw=NULL;error=AEE_EFAILED;break;}
        w->created=1;
        error=prl_multi_wait(w,&w->started,1u);
        if (!error) error=__atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
        if (error) break;
    }
    if (error) {prl_multi_finish(p,1);return error;}
    return 0;
}

static inline __attribute__((always_inline))
int prl_grouped_acquire_known(prl_multi_worker *w,uint32_t *tag,uint32_t *raw_offset) {
    const uint32_t i=w->produced;
    if (i>=w->count) return AEE_EBADSIZE;
    const int rc=prl_multi_wait(w,&w->slot[i%PRL_GROUPED_SLOTS].seq,i);
    if (!rc) {
        *tag=i;
        *raw_offset=w->raw_offset+(i%PRL_GROUPED_SLOTS)*PRL_GROUPED_SLOT_BYTES;
    }
    return rc;
}
static inline __attribute__((always_inline))
void prl_grouped_publish_known(prl_multi_worker *w,uint32_t tag,int rb,int cb,int first_s) {
    prl_multi_slot *slot=&w->slot[tag%PRL_GROUPED_SLOTS];
    slot->rb=rb;slot->cb=cb;slot->s=first_s;
    prl_multi_complete();
    __atomic_store_n(&slot->seq,tag+1u,__ATOMIC_RELEASE);
    w->produced=tag+1u;
}
#endif
