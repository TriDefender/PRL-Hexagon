/* One unique clock client per compound RPC; each frozen kernel chunk remains
 * at most thirty-two independent M2048 attempts with an isolated thirty-two-key internal ABI. */
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include "compound.h"
#include "protocol.h"
#include "compact_format.h"
#include "kernel_api.h"
#include "snapshot/fused_power_vote.h"
#include "HAP_perf.h"

extern int HAP_power_get(void *,HAP_power_response_t *) __attribute__((weak));
static prl_hmx_vote g_vote;
static unsigned g_call_active;
static int g_poison;
static void complete(void) {__asm__ volatile("syncht":::"memory");}
static uint64_t get64(const uint32_t *d,unsigned i) {return d[i]|((uint64_t)d[i+1]<<32);}
static void put64(uint32_t *d,unsigned i,uint64_t x) {d[i]=(uint32_t)x;d[i+1]=(uint32_t)(x>>32);}
static int clocks(uint32_t *d,unsigned at) {
    HAP_power_response_t r;int rc;
    memset(&r,0,sizeof(r));r.type=HAP_power_get_clk_Freq;
    rc=HAP_power_get?HAP_power_get(NULL,&r):HAP_POWER_ERR_UNSUPPORTED_API;
    d[at]=(uint32_t)rc;d[at+1]=rc?0:r.clkFreqHz;
    memset(&r,0,sizeof(r));r.type=HAP_power_get_hmx_core_clk_Freq;
    const int hrc=HAP_power_get?HAP_power_get(NULL,&r):HAP_POWER_ERR_UNSUPPORTED_API;
    d[at+2]=(uint32_t)hrc;d[at+3]=hrc?0:r.clkFreqHz;
    return rc||hrc?AEE_EFAILED:0;
}

int prl_compound_batch_run(int attempts,int n,int flags,int output_mode,
                          const byte *X,int XLen,const byte *W,int WLen,
                          const int *keys_and_bound,int paramsLen,
                          int *result,int resultLen,uint64 *cycles) {
    if (!cycles) return AEE_EBADPARM;
    *cycles=0;
    if ((attempts!=1&&attempts!=2&&attempts!=4&&attempts!=8&&attempts!=16&&attempts!=32)||
        n<64||n>2048||n%64||(output_mode!=1&&output_mode!=2)||flags<0||flags>131071)
        return AEE_EBADPARM;
    const int required=2|32|64|256|2048,prepare=(flags&8)!=0;
    const int compact=(flags&PRL_COMPACT_FLAG)!=0;
    if (compact && (attempts!=32 || prepare || ((flags&65535)&~4)!=35698)) return AEE_EBADPARM;
    /* Signed-RM whole-M repacking cannot reinterpret a concatenation of per-attempt slice-major arrays. */
    if ((flags&required)!=required||!(flags&1536)||(flags&128)||(flags&8192))
        return AEE_EBADPARM;
    const size_t per_attempt=(size_t)32*(unsigned)(n/64)*(output_mode==1?256u:1u);
    const size_t words=(size_t)attempts*per_attempt;
    const size_t input=compact?(size_t)PRL_COMPACT_TOTAL_BYTES(attempts):(size_t)attempts*PRL_COMPOUND_ATTEMPT_BYTES;
    if (!result||resultLen<0||(size_t)resultLen<words+64u||
        ((uintptr_t)result&(output_mode==1?127u:3u))||
        !keys_and_bound||paramsLen<attempts*8+8||XLen<0||WLen<0||
        (!prepare&&(!X||(size_t)XLen<input||((uintptr_t)X&127u)))||
        (WLen&&(!W||(size_t)WLen<(size_t)n*2048u))||((flags&4)&&!WLen))
        return AEE_EBADSIZE;
    unsigned expected=0;
    if (!__atomic_compare_exchange_n(&g_call_active,&expected,1u,0,
                                     __ATOMIC_ACQ_REL,__ATOMIC_RELAXED))
        return AEE_EBADSTATE;
    uint32_t summary[64]={0};
    summary[CB_MAGIC]=PRL_COMPOUND_MAGIC;summary[CB_VERSION]=PRL_COMPOUND_VERSION;
    summary[CB_ATTEMPTS]=(unsigned)attempts;summary[CB_N]=(unsigned)n;
    summary[CB_FLAGS]=(unsigned)flags;summary[CB_MODE]=(unsigned)output_mode;
    summary[CB_CHUNKS_PLANNED]=prepare?1u:((unsigned)attempts+31u)/32u;
    summary[CB_POLICY]=1;summary[CB_PREPARE]=(unsigned)prepare;
    summary[CB_OUTPUT_WORDS]=(uint32_t)words;summary[CB_WORDS_PER_ATTEMPT]=(uint32_t)per_attempt;
    summary[CB_MAX_CHUNK]=32;
    int rc=AEE_EFAILED;
    uint64_t core_ticks=0,core_pcycles=0,kernel_ticks=0,qt0=0,pc0=0;
    if (g_poison||g_vote.poisoned||g_vote.context||prl_compound_kernel_poisoned()) {
        g_poison=1;goto publish;
    }
    rc=prl_compound_kernel_initialize();summary[CB_INIT_RC]=(uint32_t)rc;
    if (rc) goto publish;
    qt0=HAP_perf_get_qtimer_count();pc0=HAP_perf_get_pcycles();
    if (!prepare) {
        rc=prl_hmx_vote_begin(&g_vote,1);summary[CB_BEGIN_RC]=(uint32_t)rc;
        const int clock_rc=clocks(summary,CB_PRE_Q6_RC);
        if (!rc) rc=clock_rc;
        if (rc) goto finish_vote;
    }
    for (unsigned base=0;base<(unsigned)attempts;) {
        const unsigned count=(unsigned)attempts-base<32u?(unsigned)attempts-base:32u;
        const size_t chunk_words=(size_t)count*per_attempt;
        prl_fused_fused_HashParams params;
        memset(&params,0,sizeof(params));
        memcpy(params.keys,keys_and_bound+(size_t)base*8u,(size_t)count*32u);
        memcpy(params.bound,keys_and_bound+(size_t)attempts*8u,32u);
        const int chunk_flags=base?(flags&~4):flags;
        const byte *chunk_x=compact?X:(X?X+(size_t)base*PRL_COMPOUND_ATTEMPT_BYTES:NULL);
        const byte *chunk_w=base?NULL:W;
        uint64 chunk_cycles=0;
        rc=prl_compound_kernel_run_hash((int)count*2048,n,chunk_flags,output_mode,
            chunk_x,prepare?0:(compact?XLen:(int)((size_t)count*PRL_COMPOUND_ATTEMPT_BYTES)),
            chunk_w,base?0:WLen,&params,result+(size_t)base*per_attempt,
            (int)(chunk_words+64u),&chunk_cycles);
        summary[CB_CORE_RC]=(uint32_t)rc;
        if (prl_compound_kernel_poisoned()) g_poison=1;
        if (rc) break;
        /* The temporary tail may overlap the next chunk's first 64 result words. */
        uint32_t diag[64];
        memcpy(diag,result+(size_t)base*per_attempt+chunk_words,sizeof(diag));
        core_ticks+=get64(diag,56);kernel_ticks+=get64(diag,58);
        core_pcycles+=get64(diag,14);
        summary[CB_SUM_STAGE_PCY]+=(uint32_t)get64(diag,6);
        summary[CB_SUM_MM_PCY]+=(uint32_t)get64(diag,8);
        summary[CB_SUM_READ_PCY]+=(uint32_t)get64(diag,10);
        summary[CB_SUM_FOLD_PCY]+=(uint32_t)get64(diag,12);
        summary[CB_SUM_QUEUE_ACQUIRE_WAIT_PCY]+=(uint32_t)get64(diag,18);
        summary[CB_SUM_TILE_LOOP_PCY]+=diag[38];
        summary[CB_SUM_LIFECYCLE_PCY]+=diag[39];summary[CB_CHUNKS_DONE]++;
        if (prepare) break;
        summary[CB_ATTEMPTS_DONE]+=count;base+=count;
    }
    if (g_poison&&!rc) rc=AEE_EFAILED;
finish_vote:
    complete(); /* Also drains a failed or unpublished final kernel group. */
    if (!prepare) {
        const int clock_rc=clocks(summary,CB_POST_Q6_RC);
        if (!rc) rc=clock_rc;
        const int end_rc=prl_hmx_vote_end(&g_vote);
        summary[CB_END_RC]=(uint32_t)end_rc;
        if (!rc) rc=end_rc;
        if (end_rc||g_vote.poisoned||g_vote.context) g_poison=1;
        prl_hmx_vote_write_diag(&g_vote,summary+CB_VOTE_DIAG);
    }
    const uint64_t outer_ticks=HAP_perf_get_qtimer_count()-qt0;
    const uint64_t outer_pcycles=HAP_perf_get_pcycles()-pc0;
    summary[CB_OUTER_TICKS]=(uint32_t)outer_ticks;
    summary[CB_OUTER_PCYCLES]=(uint32_t)outer_pcycles;
    if (!prepare) *cycles=outer_pcycles;
publish:
    summary[CB_KERNEL_POISON]=(uint32_t)prl_compound_kernel_poisoned();
    if (summary[CB_KERNEL_POISON]) g_poison=1;
    summary[CB_POISON]=(uint32_t)g_poison;
    if (g_poison&&!rc) rc=AEE_EFAILED;
    if (!prepare&&summary[CB_VOTE_DIAG]==0)
        prl_hmx_vote_write_diag(&g_vote,summary+CB_VOTE_DIAG);
    summary[CB_CORE_TICKS]=(uint32_t)core_ticks;
    summary[CB_SUM_CORE_PCYCLES]=(uint32_t)core_pcycles;
    summary[CB_SUM_KERNEL_TOTAL_TICKS]=(uint32_t)kernel_ticks;
    memcpy(result+words,summary,sizeof(summary));
    __atomic_store_n(&g_call_active,0u,__ATOMIC_RELEASE);
    return rc;
}
