/* Scalar seq words live outside VTCM; SYNCHT precedes both ownership changes. */
#ifndef PRL_FUSED_STAGE_H
#define PRL_FUSED_STAGE_H
#include "fused_multi.h"
#ifndef PRL_STAGE_HOST_SIM
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"
#include "fused_noise_compact.h"
#endif

#define PRL_STAGE_BLOCK_BYTES 131072u
typedef struct {
    prl_multi_worker sync;
    const uint8_t *source;
    uint8_t *vtcm;
    uint32_t offset[2],seq[2],blocks,source_done,compact;
    void *allocation;
} prl_stage;

static prl_stage *prl_stage_alloc(void) {
    void *allocation=malloc(sizeof(prl_stage)+63u);
    if (!allocation) return NULL;
    prl_stage *p=(prl_stage *)(((uintptr_t)allocation+63u)&~(uintptr_t)63u);
    memset(p,0,sizeof(*p));p->allocation=allocation;
    return p;
}
static void prl_stage_free(prl_stage *p) {
    if (p&&!p->sync.created) free(p->allocation);
}

static void prl_stage_main(void *arg) {
    prl_stage *p=(prl_stage *)arg;
    prl_multi_worker *w=&p->sync;
    const int lock=qurt_hvx_lock(QURT_HVX_MODE_128B);
    __atomic_store_n(&w->lock_rc,lock,__ATOMIC_RELEASE);
    __atomic_store_n(&w->status,lock?AEE_EFAILED:0,__ATOMIC_RELEASE);
    __atomic_store_n(&w->started,1,__ATOMIC_RELEASE);
    if (lock) {
        __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
        __atomic_store_n(&p->source_done,1,__ATOMIC_RELEASE);
        qurt_thread_exit(lock);return;
    }
    for (uint32_t rb=0;rb<p->blocks;rb++) {
        if (prl_multi_wait(w,p->seq+(rb&1u),rb)) break;
        HVX_Vector *dst=(HVX_Vector *)(p->vtcm+p->offset[rb&1u]);
#ifndef PRL_STAGE_HOST_SIM
        if (p->compact) prl_noise_make_block(p->source,rb,(uint8_t *)dst);
        else
#endif
        {
            const HVX_Vector *src=(const HVX_Vector *)(p->source+(size_t)rb*PRL_STAGE_BLOCK_BYTES);
            for (unsigned i=0;i<PRL_STAGE_BLOCK_BYTES/128u;i++) {
                dst[i]=src[i];
#ifdef PRL_STAGE_HOST_COPY_HOOK
                PRL_STAGE_HOST_COPY_HOOK(rb,i);
#endif
            }
        }
        prl_multi_complete();
        __atomic_store_n(p->seq+(rb&1u),rb+1u,__ATOMIC_RELEASE);
        __atomic_store_n(&w->processed,rb+1u,__ATOMIC_RELEASE);
    }
    /* No further RPC input access is allowed after source_done. */
    prl_multi_complete();
    __atomic_store_n(&p->source_done,1,__ATOMIC_RELEASE);
    const int unlock=qurt_hvx_unlock();
    __atomic_store_n(&w->unlock_rc,unlock,__ATOMIC_RELEASE);
    if (unlock) {
        int empty=0;
        __atomic_compare_exchange_n(&w->status,&empty,AEE_EFAILED,0,
                                    __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    }
    qurt_thread_exit(__atomic_load_n(&w->status,__ATOMIC_ACQUIRE));
}

/* The caller owns p even on failure, and must finish/join before freeing it. */
static int prl_stage_start_mode(prl_stage *p,const uint8_t *source,uint8_t *vtcm,
                           uint32_t first,uint32_t second,uint32_t blocks,unsigned compact) {
    if (!p||!source||!vtcm||!blocks||blocks>1024u||
        ((uintptr_t)source&127u)||first%2048u||second%2048u||
        (first>second?first-second:second-first)<PRL_STAGE_BLOCK_BYTES)
        return AEE_EBADPARM;
    void *allocation=p->allocation;
    memset(p,0,sizeof(*p));p->allocation=allocation;
    p->source=source;p->vtcm=vtcm;p->offset[0]=first;p->offset[1]=second;
    p->blocks=blocks;p->seq[0]=0;p->seq[1]=1;p->compact=compact;
    prl_multi_worker *w=&p->sync;
    w->stack_raw=malloc(PRL_MULTI_STACK_BYTES+127u);
    if (!w->stack_raw) return AEE_ENOMEMORY;
    void *stack=(void *)(((uintptr_t)w->stack_raw+127u)&~(uintptr_t)127u);
    qurt_thread_attr_t attr;qurt_thread_attr_init(&attr);
    qurt_thread_attr_set_detachstate(&attr,QURT_THREAD_ATTR_CREATE_JOINABLE);
    qurt_thread_attr_set_name(&attr,"prl_hvx_stage");
    qurt_thread_attr_set_stack_addr(&attr,stack);
    qurt_thread_attr_set_stack_size(&attr,PRL_MULTI_STACK_BYTES);
    const int priority=qurt_thread_get_priority(qurt_thread_get_id());
    if (priority>=0&&priority<=255) qurt_thread_attr_set_priority(&attr,(unsigned short)priority);
    if (qurt_thread_create(&w->tid,&attr,prl_stage_main,p)) {
        free(w->stack_raw);w->stack_raw=NULL;return AEE_EFAILED;
    }
    w->created=1;
    const int rc=prl_multi_wait(w,&w->started,1u);
    return rc?rc:__atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
}
static int prl_stage_start(prl_stage *p,const uint8_t *source,uint8_t *vtcm,
                           uint32_t first,uint32_t second,uint32_t blocks) {
    return prl_stage_start_mode(p,source,vtcm,first,second,blocks,0u);
}
static int prl_stage_take(prl_stage *p,uint32_t rb,uint32_t *offset) {
    if (!p||!offset||rb>=p->blocks) return AEE_EBADSIZE;
    const int rc=prl_multi_wait(&p->sync,p->seq+(rb&1u),rb+1u);
    if (!rc) *offset=p->offset[rb&1u];
    return rc;
}
static void prl_stage_release(prl_stage *p,uint32_t rb) {
    prl_multi_complete();
    __atomic_store_n(p->seq+(rb&1u),rb+2u,__ATOMIC_RELEASE);
}
static int prl_stage_finish(prl_stage *p,int abort) {
    if (!p) return 0;
    prl_multi_worker *w=&p->sync;
    if (!w->created) return __atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
    if (abort) __atomic_store_n(&w->stop,1,__ATOMIC_RELEASE);
    /* Join failure alone cannot extend the caller's input mapping lifetime. */
    while (!__atomic_load_n(&p->source_done,__ATOMIC_ACQUIRE)) qurt_sleep(1);
    int status=0;
    const int rc=qurt_thread_join(w->tid,&status);
    w->join_rc=rc;w->join_status=status;
    if (rc) return AEE_EFAILED; /* retain live stack/context; poison VTCM reuse */
    w->created=0;free(w->stack_raw);w->stack_raw=NULL;
    int error=__atomic_load_n(&w->status,__ATOMIC_ACQUIRE);
    if (!error&&status) error=status;
    if (!error&&!abort&&__atomic_load_n(&w->processed,__ATOMIC_ACQUIRE)!=p->blocks)
        error=AEE_EFAILED;
    return error;
}
#endif
