/* Test the actual compact outer RPC with fake firmware and fake inner kernel.
 * Does not model HVX, cache maintenance, real threads or HMX. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "HAP_power.h"
#include "compound.h"
#include "protocol.h"
#include "compact_format.h"
#include "kernel_api.h"

static uint64_t ticks;
static int init_calls, kernel_calls, set_calls, destroy_calls, drain_calls;
static int kernel_rc, poisoned, weights_ready;
static uint32_t fake_runtime_tag=PRL_COMPOUND_RUNTIME_TAG_MAGIC;
static uint8_t *input, *weights;
static int *params, *output;
static unsigned shape_n, shape_mode;
static size_t words;
static uint64_t HAP_perf_get_qtimer_count(void) { return ++ticks; }
static uint64_t HAP_perf_get_pcycles(void) { return ticks*100u; }
static void host_drain(void) { ++drain_calls; ++ticks; }
static void put64_host(uint32_t *d,unsigned i,uint64_t v) {
    d[i]=(uint32_t)v; d[i+1]=(uint32_t)(v>>32);
}
int HAP_power_set(void *ctx,HAP_power_request_t *req) {
    assert(ctx && req); ++set_calls; ticks+=100; return 0;
}
int HAP_power_destroy_client(void *ctx) {
    assert(ctx && drain_calls); ++destroy_calls; return 0;
}
int HAP_power_get(void *ctx,HAP_power_response_t *r) {
    assert(!ctx && r); r->clkFreqHz=1000000000u; return 0;
}
int prl_compound_kernel_initialize(void) { ++init_calls; return 0; }
int prl_compound_kernel_poisoned(void) { return poisoned; }
uint32_t prl_compound_kernel_runtime_tag(void) { return fake_runtime_tag; }
int prl_compound_kernel_run_hash(int m,int n,int flags,int mode,const byte *x,int xl,
        const byte *w,int wl,const prl_fused_fused_HashParams *p,
        int *out,int outlen,uint64 *cycles) {
    ++kernel_calls;
    assert(m==32*2048 && n==(int)shape_n && mode==(int)shape_mode);
    assert(flags==((35698|PRL_COMPACT_FLAG)|(wl?4:0)));
    assert(x==input && xl==(int)PRL_COMPACT_TOTAL_BYTES(32));
    assert(out==output && outlen==(int)words+64);
    for (unsigned i=0;i<32*8;i++) assert(p->keys[i]==(uint32_t)params[i]);
    for (unsigned i=0;i<8;i++) assert(p->bound[i]==(uint32_t)params[32*8+i]);
    if (wl) { assert(w==weights && wl==(int)shape_n*2048); weights_ready=1; }
    else assert(weights_ready);
    if (kernel_rc) return kernel_rc;
    for (size_t i=0;i<words;i++) out[i]=(int)(0x12000000u+(uint32_t)i);
    uint32_t *d=(uint32_t *)out+words;
    memset(d,0,64*sizeof(*d));
    put64_host(d,14,12345); put64_host(d,56,123); put64_host(d,58,150);
    *cycles=12345; ticks+=300; return 0;
}
#include "actual_compound.inc"

static void reset_counts(void) {
    init_calls=kernel_calls=set_calls=destroy_calls=drain_calls=0;
}
static int call(int attempts,int flags,int mode,const byte *x,int xl,
                const byte *w,int wl,int plen,int olen,uint64 *cycles) {
    return prl_compound_bytefold_batch_run(attempts,(int)shape_n,flags,mode,x,xl,w,wl,
                                       params,plen,output,olen,cycles);
}
static void clear_output(void) {
    for (size_t i=0;i<words+80;i++) output[i]=(int)0xa5a5a5a5u;
}
static void assert_untouched(void) {
    for (size_t i=0;i<words+80;i++) assert((uint32_t)output[i]==0xa5a5a5a5u);
    assert(!init_calls&&!kernel_calls&&!set_calls&&!destroy_calls&&!drain_calls);
}
static void reject(int attempts,int flags,int mode,const byte *x,int xl,
                   const byte *w,int wl,int plen,int olen) {
    uint64 cycles=99; reset_counts(); clear_output();
    assert(call(attempts,flags,mode,x,xl,w,wl,plen,olen,&cycles)!=0);
    assert(cycles==0); assert_untouched();
}
static void run_shape(unsigned n,unsigned mode,unsigned bytefold_mode) {
    shape_n=n; shape_mode=mode;
    fake_runtime_tag=PRL_COMPOUND_RUNTIME_TAG_MAGIC|bytefold_mode;
    const size_t per=(size_t)32*(n/64u)*(mode==0?512u:(mode==1?256u:1u));
    words=32u*per;
    assert(!posix_memalign((void **)&input,128,PRL_COMPACT_TOTAL_BYTES(32)));
    assert(!posix_memalign((void **)&weights,128,(size_t)n*2048));
    assert(!posix_memalign((void **)&output,128,(words+80)*sizeof(*output)));
    params=malloc((32*8+8)*sizeof(*params)); assert(params);
    for (unsigned i=0;i<32*8+8;i++) params[i]=(int)(0x12300000u+i);
    const int f=35698|PRL_COMPACT_FLAG, len=(int)PRL_COMPACT_TOTAL_BYTES(32);
    uint64 cycles=0; reset_counts(); clear_output(); weights_ready=0;
    assert(!call(32,f|4,(int)mode,input,len,weights,(int)n*2048,264,(int)words+64,&cycles));
    assert(kernel_calls==1&&init_calls==1&&set_calls==2&&destroy_calls==1&&drain_calls==1);
    assert(cycles && (uint32_t)output[0]==0x12000000u);
    uint32_t *d=(uint32_t *)output+words;
    assert(d[CB_MAGIC]==PRL_COMPOUND_MAGIC&&d[CB_FLAGS]==(uint32_t)(f|4));
    assert(d[CB_ATTEMPTS_DONE]==32&&d[CB_MAX_CHUNK]==32&&d[CB_CHUNKS_DONE]==1);
    unsigned decoded_bytefold_mode=99;
    assert(d[CB_RUNTIME_KERNEL_TAG]==fake_runtime_tag);
    assert(prl_compound_runtime_tag_mode(d[CB_RUNTIME_KERNEL_TAG],&decoded_bytefold_mode));
    assert(decoded_bytefold_mode==bytefold_mode);
    assert(prl_compound_runtime_tag_matches(d[CB_RUNTIME_KERNEL_TAG],
                                           (int)bytefold_mode,&decoded_bytefold_mode));
    assert(!prl_compound_runtime_tag_matches(d[CB_RUNTIME_KERNEL_TAG],
                                            (int)(bytefold_mode^1u),&decoded_bytefold_mode));
    assert(!prl_compound_runtime_tag_mode(0x44454501u,&decoded_bytefold_mode));
    for (size_t i=words+64;i<words+80;i++) assert((uint32_t)output[i]==0xa5a5a5a5u);
    reset_counts(); clear_output();
    assert(!call(32,f,(int)mode,input,len,NULL,0,264,(int)words+64,&cycles));
    assert(kernel_calls==1&&d[CB_FLAGS]==(uint32_t)f&&d[CB_ATTEMPTS_DONE]==32);
    reject(16,f,(int)mode,input,len,weights,(int)n*2048,264,(int)words+64);
    reject(32,f|8,(int)mode,input,len,weights,(int)n*2048,264,(int)words+64);
    reject(32,f|1,(int)mode,input,len,weights,(int)n*2048,264,(int)words+64);
    reject(32,f,(int)mode,input,len-1,weights,(int)n*2048,264,(int)words+64);
    reject(32,f,(int)mode,input+1,len-1,weights,(int)n*2048,264,(int)words+64);
    reject(32,f,(int)mode,NULL,len,weights,(int)n*2048,264,(int)words+64);
    reject(32,f,(int)mode,input,len,weights,(int)n*2048,263,(int)words+64);
    reject(32,f,(int)mode,input,len,weights,(int)n*2048,264,(int)words+63);
    reject(32,f,(int)mode,input,len,weights,(int)n*2048-1,264,(int)words+64);
    reject(32,f|4,(int)mode,input,len,NULL,0,264,(int)words+64);
    if (mode==0 || mode==1) {
        reset_counts(); clear_output(); cycles=99;
        assert(prl_compound_bytefold_batch_run(32,(int)n,f,(int)mode,input,len,weights,
               (int)n*2048,params,264,output+1,(int)words+64,&cycles)!=0);
        assert(!cycles); assert_untouched();
    }
    if (mode==0) {
        shape_n=64;
        reject(32,f,(int)mode,input,len,weights,64*2048,264,(int)words+64);
        shape_n=n;
    }
    reset_counts(); clear_output(); kernel_rc=-91;
    assert(call(32,f,(int)mode,input,len,NULL,0,264,(int)words+64,&cycles)==-91);
    assert(kernel_calls==1&&destroy_calls==1&&drain_calls==1&&!g_call_active);
    assert(((uint32_t *)output)[words+CB_CORE_RC]==(uint32_t)-91);
    kernel_rc=0; reset_counts();
    assert(!call(32,f,(int)mode,input,len,NULL,0,264,(int)words+64,&cycles));
    const uint32_t valid_tag=fake_runtime_tag;
    fake_runtime_tag=PRL_COMPOUND_RUNTIME_TAG_MAGIC|2u;
    reset_counts(); clear_output();
    assert(!call(32,f,(int)mode,input,len,NULL,0,264,(int)words+64,&cycles));
    d=(uint32_t *)output+words;
    assert(d[CB_RUNTIME_KERNEL_TAG]==fake_runtime_tag);
    assert(!prl_compound_runtime_tag_mode(d[CB_RUNTIME_KERNEL_TAG],&decoded_bytefold_mode));
    fake_runtime_tag=valid_tag;
    free(input); free(weights); free(output); free(params);
}
int main(void) {
    int parsed=-2;
    assert(prl_compound_parse_expected_bytefold_mode(NULL,&parsed)==0);
    assert(prl_compound_parse_expected_bytefold_mode("0",&parsed)&&parsed==0);
    assert(prl_compound_parse_expected_bytefold_mode("1",&parsed)&&parsed==1);
    assert(!prl_compound_parse_expected_bytefold_mode("",&parsed));
    assert(!prl_compound_parse_expected_bytefold_mode("2",&parsed));
    assert(!prl_compound_parse_expected_bytefold_mode("01",&parsed));
    assert(!prl_compound_parse_expected_bytefold_mode("1x",&parsed));
    run_shape(64,1,0); run_shape(64,1,1);
    run_shape(64,2,0); run_shape(64,2,1);
    run_shape(2048,1,0); run_shape(2048,1,1);
    run_shape(2048,2,0); run_shape(2048,2,1);
    run_shape(1024,0,0); run_shape(1024,0,1);
    puts("COMPOUND_BYTEFOLD_OUTER_HOST_PASS actual_outer=1 output_modes=0,1,2 runtime_deep4_mode=1 bytefold_modes=0,1 raw_TT=1 n=64,1024,2048 reload_and_cache=1 rejects_no_side_effect=1 error_recovery=1 runtime_tag_propagated=1 expected_mode_parser=strict invalid_tag_and_mismatch_rejected=1 HMX_simulated=0");
    return 0;
}
