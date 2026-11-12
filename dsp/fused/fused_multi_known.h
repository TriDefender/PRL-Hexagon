/* These operations retain the generic queue's exact count/sequence, timeout, completion and atomic ownership rules; only owner lookup and optional cycle accounting are omitted. */
#ifndef PRL_FUSED_MULTI_KNOWN_H
#define PRL_FUSED_MULTI_KNOWN_H
#include "fused_multi.h"

static inline __attribute__((always_inline))
int prl_multi_acquire_known_unprofiled(prl_multi_worker *w,uint32_t *tag,
                                      uint32_t *raw_offset) {
    const uint32_t i=w->produced;
    if (i>=w->count) return AEE_EBADSIZE;
    const int rc=prl_multi_wait(w,&w->slot[i%PRL_MULTI_SLOTS].seq,i);
    if (!rc) {
        *tag=i;
        *raw_offset=w->raw_offset+(i%PRL_MULTI_SLOTS)*PRL_MULTI_SLOT_BYTES;
    }
    return rc;
}

static inline __attribute__((always_inline))
void prl_multi_publish_known(prl_multi_worker *w,uint32_t tag,int rb,int cb,int s) {
    prl_multi_slot *slot=&w->slot[tag%PRL_MULTI_SLOTS];
    slot->rb=rb;slot->cb=cb;slot->s=s;
    prl_multi_complete();
    __atomic_store_n(&slot->seq,tag+1u,__ATOMIC_RELEASE);
    w->produced=tag+1u;
}
#endif
