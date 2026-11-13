/* Host-only model for running app_check itself. The first attempt uses compact
 * A' and the prepared B' to calculate all 1024 tiles on CPU. Subsequent mixed
 * masks are stubbed; all-hit masks are exact by construction. No DSP runs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "remote.h"
#include "rpcmem.h"
#include "fused.h"
#include "compound.h"
#include "protocol.h"
#include "compact_format.h"
#include "pearl.h"
#include "blake3_impl.h"

int remote_session_control(uint32_t req,void *data,uint32_t len) {
    assert(req==2&&data&&len);return 0;
}
void rpcmem_init(void) {}
void *rpcmem_alloc(int heapid,uint32 flags,int size) {
    (void)heapid;(void)flags;void *p=NULL;
    return size>0&&!posix_memalign(&p,128,(size_t)size)?p:NULL;
}
void rpcmem_free(void *p) {free(p);}
int rpcmem_to_fd(void *p) {(void)p;return -1;}
void blake3_hash_many_neon(const uint8_t *const *inputs,size_t count,size_t blocks,
        const uint32_t key[8],uint64_t counter,bool increment,uint8_t flags,
        uint8_t start,uint8_t end,uint8_t *out) {
    blake3_hash_many_portable(inputs,count,blocks,key,counter,increment,flags,start,end,out);
}
int prl_fused_fused_run_hash(int m,int n,int flags,int mode,const byte *x,int xl,
        const byte *w,int wl,const prl_fused_fused_HashParams *p,
        int *out,int ol,uint64 *cycles) {
    (void)m;(void)n;(void)flags;(void)mode;(void)x;(void)xl;(void)w;(void)wl;
    (void)p;(void)out;(void)ol;(void)cycles;return -1;
}
static int at_most(const uint8_t digest[32],const uint8_t bound[32]) {
    for (int i=31;i>=0;i--) if (digest[i]!=bound[i]) return digest[i]<bound[i];
    return 1;
}
int prl_compound_batch_run(int attempts,int n,int flags,int mode,const byte *x,int xl,
        const byte *w,int wl,const int *params,int plen,int *out,int olen,uint64 *cycles) {
    assert(attempts==32&&n==64&&flags==(35698|PRL_COMPACT_FLAG)&&mode==2);
    assert(x&&xl==(int)PRL_COMPACT_TOTAL_BYTES(32)&&w&&plen==264&&olen==1088);
    (void)wl;
    const uint8_t *bound=(const uint8_t *)(params+32*8);
    memset(out,bound[31]==0xff?0xff:0,1024*sizeof(*out));
    static unsigned mixed_calls;
    if (bound[31]!=0xff&&mixed_calls++==0) {
        int8_t *ap=malloc((size_t)2048*2048),*btp=malloc((size_t)64*2048);
        assert(ap&&btp);
        const uint8_t *r=x+PRL_COMPACT_BASE_BYTES;
        for (unsigned row=0;row<2048;row++) for (unsigned k=0;k<2048;k++) {
            unsigned p=r[PRL_COMPACT_P_OFFSET+k],q=r[PRL_COMPACT_Q_OFFSET+k];
            int fp=(int8_t)r[(size_t)row*128+2*(p&63)+(p>>6)];
            int fq=(int8_t)r[(size_t)row*128+2*(q&63)+(q>>6)];
            int base=(row==0&&k<8)?(int8_t)r[PRL_COMPACT_NONCE_OFFSET+k]:(int8_t)x[(size_t)row*2048+k];
            ap[(size_t)row*2048+k]=(int8_t)(base+fp-fq);
        }
        for (unsigned col=0;col<64;col++) for (unsigned k=0;k<2048;k++)
            btp[(size_t)col*2048+k]=(int8_t)w[((size_t)(k/128)*128+k%128)*64+col];
        for (unsigned rb=0;rb<32;rb++) for (unsigned lane=0;lane<32;lane++) {
            uint32_t t[16];uint8_t digest[32];
            pr_tile_scalar(ap,btp,2048,rb*64+lane/4,2*(lane%4),(const uint8_t *)params,t,digest);
            if (at_most(digest,bound)) ((uint32_t *)out)[rb]|=UINT32_C(1)<<lane;
        }
        free(ap);free(btp);
    }
    uint32_t *d=(uint32_t *)out+1024;memset(d,0,64*sizeof(*d));
    d[CB_MAGIC]=PRL_COMPOUND_MAGIC;d[CB_VERSION]=PRL_COMPOUND_VERSION;
    d[CB_ATTEMPTS]=32;d[CB_N]=64;d[CB_FLAGS]=(uint32_t)flags;d[CB_MODE]=2;
    d[CB_CHUNKS_PLANNED]=d[CB_CHUNKS_DONE]=1;d[CB_ATTEMPTS_DONE]=32;d[CB_POLICY]=1;
    d[CB_PRE_Q6_HZ]=d[CB_PRE_HMX_HZ]=d[CB_POST_Q6_HZ]=d[CB_POST_HMX_HZ]=1000000000u;
    d[CB_OUTPUT_WORDS]=1024;d[CB_WORDS_PER_ATTEMPT]=32;d[CB_MAX_CHUNK]=32;
    d[CB_CORE_TICKS]=100;d[CB_OUTER_TICKS]=110;d[CB_OUTER_PCYCLES]=12345;
    const char *tag_mode=getenv("PRL_EXPECT_BYTEFOLD_EARLY");
    assert(tag_mode&&(tag_mode[0]=='0'||tag_mode[0]=='1')&&tag_mode[1]=='\0');
    d[CB_SUM_CORE_PCYCLES]=10000;d[63]=UINT32_C(0x42464500)|(uint32_t)(tag_mode[0]-'0');
    *cycles=12345;return 0;
}
