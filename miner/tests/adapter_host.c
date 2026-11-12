/* RPC and result masks are simulated; no HMX is modeled. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "remote.h"
#include "rpcmem.h"
#include "fused.h"
#include "compound.h"
#include "protocol.h"
#include "compact_format.h"
#include "pearl.h"
#include "blake3_impl.h"

static int8_t *expected_base;
static uint64_t expected_first;
static int rpc_calls, weight_loads, force_failure;
static const uint64_t nonce_mask=(UINT64_C(1)<<56)-1;
int remote_session_control(uint32_t req,void *data,uint32_t len) {
    assert(req==2 && data && len); return 0;
}
void rpcmem_init(void) {}
void *rpcmem_alloc(int heapid,uint32 flags,int size) {
    (void)heapid; (void)flags; void *p=NULL;
    return size>0 && !posix_memalign(&p,128,(size_t)size) ? p : NULL;
}
void rpcmem_free(void *p) { free(p); }
int rpcmem_to_fd(void *p) { (void)p; return -1; }
void blake3_hash_many_neon(const uint8_t *const *inputs,size_t count,size_t blocks,
        const uint32_t key[8],uint64_t counter,bool increment,uint8_t flags,
        uint8_t start,uint8_t end,uint8_t *out) {
    blake3_hash_many_portable(inputs,count,blocks,key,counter,increment,flags,start,end,out);
}
int prl_fused_fused_run_hash(int m,int n,int f,int mode,const byte *x,int xl,
        const byte *w,int wl,const prl_fused_fused_HashParams *p,
        int *o,int ol,uint64 *cycles) {
    (void)m;(void)n;(void)f;(void)mode;(void)x;(void)xl;(void)w;(void)wl;
    (void)p;(void)o;(void)ol;(void)cycles; return -1;
}
int prl_compound_batch_run(int attempts,int n,int flags,int mode,const byte *x,int xl,
        const byte *w,int wl,const int *params,int plen,int *out,int olen,uint64 *cycles) {
    assert(attempts==32 && n==64 && flags==(35698|PRL_COMPACT_FLAG) && mode==2);
    assert(x && xl==(int)PRL_COMPACT_TOTAL_BYTES(32) && plen==264);
    assert(w && (wl==0||wl==64*2048));
    if (wl) ++weight_loads;
    assert(!memcmp(x,expected_base,PRL_COMPACT_BASE_BYTES));
    for (unsigned a=0;a<32;a++) {
        const uint8_t *r=x+PRL_COMPACT_BASE_BYTES+(size_t)a*PRL_COMPACT_RECORD_BYTES;
        static const uint8_t label[32]={'A','_','t','e','n','s','o','r'};
        const uint8_t *key=(const uint8_t *)(params+a*8);
        const unsigned samples[3]={0,1023,2047};
        for (unsigned z=0;z<3;z++) {
            uint32_t pair[2];int8_t factor[128];
            pr_perm_pair(label,key,samples[z],128,pair);
            pr_uniform_row(label,key,samples[z],128,factor);
            assert(r[PRL_COMPACT_P_OFFSET+samples[z]]==pair[0]);
            assert(r[PRL_COMPACT_Q_OFFSET+samples[z]]==pair[1]);
            for (unsigned k=0;k<2;k++) {
                unsigned ix=pair[k];
                assert((int8_t)r[(size_t)samples[z]*128+2*(ix&63)+(ix>>6)]==factor[ix]);
            }
        }
        const uint64_t nonce=(expected_first+a)&nonce_mask;
        for (unsigned j=0;j<8;j++)
            assert(r[PRL_COMPACT_NONCE_OFFSET+j]==(uint8_t)((int)((nonce>>(7*j))&127)-64));
    }
    if (force_failure) {++rpc_calls; return -91;}
    const unsigned per=32, words=attempts*per;
    assert(olen==(int)words+64);
    for (unsigned i=0;i<words;i++) out[i]=-1;
    uint32_t *d=(uint32_t *)out+words; memset(d,0,64*sizeof(*d));
    d[CB_MAGIC]=PRL_COMPOUND_MAGIC;d[CB_VERSION]=PRL_COMPOUND_VERSION;
    d[CB_ATTEMPTS]=32;d[CB_N]=64;d[CB_FLAGS]=(uint32_t)flags;d[CB_MODE]=2;
    d[CB_CHUNKS_PLANNED]=d[CB_CHUNKS_DONE]=1;d[CB_ATTEMPTS_DONE]=32;
    d[CB_POLICY]=1;d[CB_PRE_Q6_HZ]=d[CB_PRE_HMX_HZ]=1000000000u;
    d[CB_POST_Q6_HZ]=d[CB_POST_HMX_HZ]=1000000000u;
    d[CB_OUTPUT_WORDS]=words;d[CB_WORDS_PER_ATTEMPT]=per;
    d[CB_MAX_CHUNK]=32;d[CB_CORE_TICKS]=100;d[CB_OUTER_TICKS]=110;
    d[CB_OUTER_PCYCLES]=12345;d[CB_SUM_CORE_PCYCLES]=10000;
    *cycles=12345;++rpc_calls;return 0;
}
#include "actual_qnpu.inc"

int main(void) {
    setenv("PRL_PERSIST_MAP","0",1);
    char err[256];
    pr_qnpu_ctx *c=pr_qnpu_new(2048,64,2048,err,sizeof(err));
    if (!c) {fprintf(stderr,"ADAPTER_HOST_FAIL init %s\n",err);return 1;}
    assert(pr_qnpu_compound_attempts(c)==32);
    pr_attempt_mats am={.m=2048,.n=64,.k=2048};
    am.a=malloc((size_t)2048*2048); am.bt=malloc((size_t)64*2048);
    assert(am.a&&am.bt); expected_base=malloc((size_t)2048*2048);assert(expected_base);
    for (unsigned job=0;job<2;job++) {
        uint8_t header[76],bound[32]; memset(bound,0xff,sizeof(bound));
        for (unsigned i=0;i<76;i++) header[i]=(uint8_t)(job*53+i*17);
        const uint64_t seed=job?2026:1;
        pr_fill_base(&am,seed);memcpy(expected_base,am.a,(size_t)2048*2048);
        pr_qnpu_set_base_seed(c,seed);assert(!pr_qnpu_set_job(c,header,err,sizeof(err)));
        pr_qnpu_set_bound(c,bound);
        const uint64_t start=job?617:nonce_mask-15;
        for (unsigned batch=0;batch<2;batch++) {
            expected_first=(start+batch*32)&nonce_mask;
            assert(!pr_qnpu_compound_pipe(c,expected_first));
            for (unsigned a=0;a<32;a++) {
                uint64_t nonce,coords[PRL_QNPU_MAXHITS];
                assert(pr_qnpu_compound_result(c,a,&nonce,coords,NULL)==1024);
                assert(nonce==((expected_first+a)&nonce_mask));
                for (unsigned j=0;j<8;j++) am.a[j]=(int8_t)((int)((nonce>>(7*j))&127)-64);
                pr_commitment expected;pr_commit(header,2048,am.a,am.bt,2048,64,&expected);
                assert(!memcmp(&expected,pr_qnpu_commitment(c),sizeof(expected)));
                assert(!memcmp(am.a,pr_qnpu_host_a(c),(size_t)2048*2048));
                assert(!memcmp(am.bt,pr_qnpu_host_bt(c),(size_t)64*2048));
                for (unsigned i=0;i<1024;i++) {
                    unsigned row=(i/32)*64+(i%32)/4,col=2*(i%4);
                    assert(coords[i]==((uint64_t)row|((uint64_t)col<<32)));
                }
            }
        }
        (void)pr_qnpu_drain(c,NULL,NULL);
    }
    assert(rpc_calls==4&&weight_loads==2);
    assert(pr_qnpu_macs(c)==(uint64_t)4*32*2048*64*2048);
    /* A failed RPC must retire the compound path even when the next slot is
     * being prepared; teardown must join the real preparation workers. */
    force_failure=1;expected_first=900;
    assert(pr_qnpu_compound_pipe(c,expected_first)!=0);
    assert(!pr_qnpu_compound_available(c));
    assert(rpc_calls==5);
    (void)pr_qnpu_drain(c,NULL,NULL);
    pr_qnpu_free(c);free(expected_base);free(am.a);free(am.bt);
    puts("COMPACT_ADAPTER_HOST_PASSED real_adapter=1 real_workers=1 real_commitments=1 real_noise_record=1 scalar_NEON_model=1 mocked_rpc=1 jobs=2 batches=4 attempts=128 nonce_wrap=1 rpc_failure_cleanup=1 HMX_simulated=0");
    return 0;
}
