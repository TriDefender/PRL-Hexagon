/* The documented QuRT DMSYNCHT operation completes posted DMA memory ops before the producer lets HMX read the VTCM destination. */
#ifndef PRL_FUSED_DMA_H
#define PRL_FUSED_DMA_H
#include <stdint.h>
#include <string.h>
#include "AEEStdErr.h"
#include "qurt_user_dma.h"
#include "dma_utils.h"
#include "dma_desc.h"
#include "dma_regs.h"
#include "dma_cmds.h"

#pragma weak qurt_user_dma_dmsyncht

#define PRL_DMA_BLOCK_BYTES (16u*64u*128u)
#define PRL_DMA_DESCRIPTOR_STRIDE 128u
#define PRL_DMA_DESCRIPTOR_BYTES 2048u

typedef struct {
    uint8_t *descriptors;
    int active,api_present,probe_rc;
} prl_dma;

static int prl_dma_init(prl_dma *p,uint8_t *descriptors) {
    memset(p,0,sizeof(*p));
    p->api_present=qurt_user_dma_dmsyncht!=NULL;
    p->probe_rc=0;
    if ((uintptr_t)descriptors&127u) return AEE_EBADPARM;
    if (!p->api_present) return AEE_EUNSUPPORTED;
    /* Do not take over an already running/error DMA chain from the caller's thread. */
    p->probe_rc=qurt_user_dma_dmsyncht();
    if (p->probe_rc) return AEE_EUNSUPPORTED;
    if ((dmpoll()&DM0_STATUS_MASK)!=DM0_STATUS_IDLE) return AEE_EFAILED;
    p->descriptors=descriptors;
    return 0;
}
static int prl_dma_submit(prl_dma *p,unsigned parity,const uint8_t *src,
                           uint8_t *dst) {
    if (p->active || !p->descriptors || parity>1 ||
        (((uintptr_t)src|(uintptr_t)dst)&127u)) return AEE_EBADPARM;
    if ((dmpoll()&DM0_STATUS_MASK)!=DM0_STATUS_IDLE) return AEE_EFAILED;
    uint8_t *desc=p->descriptors+parity*PRL_DMA_DESCRIPTOR_STRIDE;
    for (unsigned word=0;word<DMA_DESC_SIZE_1D/4u;word++)
        ((volatile uint32_t *)desc)[word]=0;
    dma_desc_1d_params_t params={
        .src_address=(uint32_t)(uintptr_t)src,
        .dst_address=(uint32_t)(uintptr_t)dst,
        .length=PRL_DMA_BLOCK_BYTES,
        .order=DESC_ORDER_ORDER,
        .src_bypass=DESC_BYPASS_OFF,
        .dst_bypass=DESC_BYPASS_ON
    };
    int rc=dma_desc_init(desc,&params,DMA_DESC_TYPE_1D);
    if (rc!=DMA_SUCCESS) return AEE_EFAILED;
    void *batch[1]={desc};
    __asm__ volatile("" ::: "memory");
    rc=dma_desc_submit(batch,1);
    if (rc!=DMA_SUCCESS) return AEE_EFAILED;
    p->active=1;
    return 0;
}
static int prl_dma_drain(prl_dma *p) {
    if (!p->active) return 0;
    const int idle=dma_wait_for_idle();
    const int sync=qurt_user_dma_dmsyncht();
    /* Successful DMSYNCHT proves there are no remaining posted memory ops, including on the error path. */
    if (!sync) p->active=0;
    return idle==DMA_SUCCESS&&!sync?0:AEE_EFAILED;
}
#endif
