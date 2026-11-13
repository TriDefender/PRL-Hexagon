/* Protocol-stage benchmark: compare every tile's keyed BLAKE3 digest or hit
 * mask against the official portable implementation and the full CPU TT.
 * Reuse the transcript benchmark's independent input/reference helpers. */
#define PRL_TRANSCRIPT_HELPERS_ONLY 1
#include "fused_driver.c"
#include "blake3.h"
#include <stdbool.h>

static uint32_t load_le32(const uint8_t *p) {
    return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void store_le32(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static int at_most(const uint32_t a[8],const uint32_t b[8]) {
    for (int w=7;w>=0;w--) if (a[w]!=b[w]) return a[w]<b[w];
    return 1;
}
static void cpu_digests(const uint32_t *tt,const uint32_t *keys,uint32_t *dig,int m,int n) {
    const size_t bands=(size_t)(m/64)*(n/64);
    for (size_t b=0;b<bands;b++) for (unsigned lane=0;lane<32;lane++) {
        const size_t attempt=(b/(n/64))/32;
        uint8_t key[32],msg[64],digest[32];
        for (int w=0;w<8;w++) store_le32(key+w*4,keys[attempt*8+w]);
        for (int s=0;s<16;s++) store_le32(msg+s*4,tt[(b*32+lane)*16+s]);
        blake3_hasher h;
        blake3_hasher_init_keyed(&h,key);
        blake3_hasher_update(&h,msg,64);
        blake3_hasher_finalize(&h,digest,32);
        for (int w=0;w<8;w++) dig[b*256+w*32+lane]=load_le32(digest+w*4);
    }
}
/* Official upstream four-way NEON backend, used only for a stronger ARM
 * baseline. The independent reference above remains the portable hasher. */
extern void blake3_hash_many_neon(const uint8_t *const *,size_t,size_t,
    const uint32_t[8],uint64_t,bool,uint8_t,uint8_t,uint8_t,uint8_t *);
static void cpu_digests_neon(const uint32_t *tt,const uint32_t *keys,uint32_t *dig,int m,int n) {
    const size_t bands=(size_t)(m/64)*(n/64);
    for (size_t b=0;b<bands;b++) {
        const uint8_t *messages[32];uint8_t result[32*32];
        for (unsigned lane=0;lane<32;lane++) messages[lane]=(const uint8_t *)(tt+(b*32+lane)*16);
        const size_t attempt=(b/(n/64))/32;
        blake3_hash_many_neon(messages,32,1,keys+attempt*8,0,false,24,1,2,result);
        for (int w=0;w<8;w++) for (unsigned lane=0;lane<32;lane++)
            dig[b*256+w*32+lane]=load_le32(result+lane*32+w*4);
    }
}
static size_t cpu_masks(const uint32_t *dig,const uint32_t bound[8],uint32_t *mask,size_t bands) {
    size_t hits=0;
    for (size_t b=0;b<bands;b++) {
        uint32_t bits=0;
        for (unsigned lane=0;lane<32;lane++) {
            uint32_t d[8];
            for (int w=0;w<8;w++) d[w]=dig[b*256+w*32+lane];
            if (at_most(d,bound)) {bits|=UINT32_C(1)<<lane;hits++;}
        }
        mask[b]=bits;
    }
    return hits;
}
#ifndef PRL_HASH_HELPERS_ONLY
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const int m=argc>1?atoi(argv[1]):2048,n=argc>2?atoi(argv[2]):1024;
    const int reps=argc>3?atoi(argv[3]):50,flags=argc>4?atoi(argv[4]):2930;
    const int mode=argc>5?atoi(argv[5]):1;
    const char *bound_case=argc>6?argv[6]:"random";
    if (m<64||m>8192||n<64||n>2048||m%64||n%64||reps<1||reps>100000||mode<1||mode>2) return 2;
    if (strcmp(bound_case,"random")&&strcmp(bound_case,"all")&&strcmp(bound_case,"none")&&
        strcmp(bound_case,"equal")&&strcmp(bound_case,"below")&&strcmp(bound_case,"above")) return 2;
    cpu_set_t cs;CPU_ZERO(&cs);CPU_SET(6,&cs);
    if (sched_setaffinity(0,sizeof(cs),&cs)) return 1;
    struct remote_rpc_control_unsigned_module req={.domain=3,.enable=1};
    int rc=remote_session_control(2,&req,sizeof(req));
    if (rc) {printf("FAIL unsigned rc=%d\n",rc);return 1;}
    const int serial=getenv("PRL_RPC_SERIAL")?atoi(getenv("PRL_RPC_SERIAL")):0;
    if (serial&&(!remote_set_mode||remote_set_mode(REMOTE_MODE_SERIAL))) return 1;
    g_persist_map=getenv("PRL_PERSIST_MAP")?atoi(getenv("PRL_PERSIST_MAP")):(remote_register_buf_attr2!=NULL);
    g_pack_rows=1;g_signed_rm=(flags&8192)!=0;
    uint64_t seed=getenv("PRL_SEED")?strtoull(getenv("PRL_SEED"),NULL,0):2026;
    const int full=getenv("PRL_FULL_INT8")?atoi(getenv("PRL_FULL_INT8")):1;
    const int repack=getenv("PRL_REPACK")?atoi(getenv("PRL_REPACK")):1;
    const int neon=getenv("PRL_HASH_NEON")?atoi(getenv("PRL_HASH_NEON")):0;
    const int baseline=getenv("PRL_HASH_BASELINE")?atoi(getenv("PRL_HASH_BASELINE")):0;
    const size_t xs=(size_t)16*m*128,ws=(size_t)16*n*128;
    const size_t bands=(size_t)(m/64)*(n/64),ttwords=bands*512,digwords=bands*256;
    const size_t outwords=mode==1?digwords:bands,keywords=(size_t)((m+2047)/2048)*8;
    int8_t *a[2],*wj=malloc(ws);
    const size_t xalloc=xs,outalloc=(outwords+64)*4;
    prl_fused_fused_HashParams params={0};
    uint8_t *x=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)xalloc);
    int8_t *w=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)ws);
    uint32_t *out=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)outalloc);
    uint32_t *dg=out+outwords;
    uint32_t *keys=(uint32_t *)params.keys;
    uint32_t *bound=(uint32_t *)params.bound;
    uint32_t *keyref[2],*ttref[2],*digref[2],*maskref[2];
    uint32_t *base_tt=baseline?rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)((ttwords+64)*4)):NULL;
    uint32_t *base_dig=baseline?malloc(digwords*4):NULL,*base_mask=baseline?malloc(bands*4):NULL;
    if (!wj||!x||!w||!out||!dg||!keys||!bound||(baseline&&(!base_tt||!base_dig||!base_mask))) return 1;
    if (map_persistent(x,xalloc)||map_persistent(w,ws)||map_persistent(out,outalloc)||
        (baseline&&map_persistent(base_tt,(ttwords+64)*4))) return 1;
    for (int h=0;h<2;h++) {
        a[h]=malloc(xs);keyref[h]=malloc(keywords*4);ttref[h]=calloc(ttwords,4);
        digref[h]=malloc(digwords*4);maskref[h]=malloc(bands*4);
        if (!a[h]||!keyref[h]||!ttref[h]||!digref[h]||!maskref[h]) return 1;
        for (size_t i=0;i<xs;i++) {unsigned z=(unsigned)rng(&seed);a[h][i]=(int8_t)(full?(z&255):(z&127)-64);}
        for (size_t i=0;i<keywords;i++) keyref[h][i]=(uint32_t)rng(&seed);
    }
    for (size_t i=0;i<ws;i++) {unsigned z=(unsigned)rng(&seed);w[i]=(int8_t)(full?(z&255):(z&127)-64);}
    for (int s=0;s<16;s++) for (int j=0;j<n;j++) for (int k=0;k<128;k++)
        wj[((size_t)s*n+j)*128+k]=w[((size_t)s*128+k)*n+j];
    double r0=now_ms();
    for (int h=0;h<2;h++) {
        if (reference(a[h],wj,ttref[h],m,n,0)) return 1;
        cpu_digests(ttref[h],keyref[h],digref[h],m,n);
    }
    for (int k=0;k<8;k++) bound[k]=(uint32_t)rng(&seed);
    if (!strcmp(bound_case,"all")) for (int k=0;k<8;k++) bound[k]=UINT32_MAX;
    if (!strcmp(bound_case,"none")) memset(bound,0,32);
    if (!strcmp(bound_case,"equal")||!strcmp(bound_case,"below")||!strcmp(bound_case,"above")) {
        for (int k=0;k<8;k++) bound[k]=digref[0][k*32+31];
        if (!strcmp(bound_case,"below")) for (int k=0;k<8;k++) if (bound[k]--!=0) break;
        if (!strcmp(bound_case,"above")) for (int k=0;k<8;k++) if (++bound[k]!=0) break;
    }
    size_t hits[2];
    for (int h=0;h<2;h++) hits[h]=cpu_masks(digref[h],bound,maskref[h],bands);
    printf("HASH config m=%d n=%d reps=%d flags=%d mode=%d bound=%s keys=%zu output_bytes=%zu baseline=%d repack=%d persist=%d serial=%d inline_params=1 ARM_hash=%s\n",
        m,n,reps,flags,mode,bound_case,keywords/8,outwords*4,baseline,repack,g_persist_map,serial,neon?"official_NEON4":"official_portable");
    printf("HASH reference full CPU TT + official keyed BLAKE3 ready %.3f ms, expected_hits=%zu/%zu\n",now_ms()-r0,hits[0],hits[1]);
    pack(a[0],x,m);memcpy(keys,keyref[0],keywords*4);
    uint64 cycles=0;
    if (baseline) rc=prl_fused_fused_run(m,n,flags|4|8,(byte *)x,(int)xs,(byte *)w,(int)ws,
        (int *)base_tt,(int)(ttwords+64),&cycles);
    else rc=prl_fused_fused_run_hash(m,n,flags|4|8,mode,(byte *)x,(int)xs,(byte *)w,(int)ws,
        &params,(int *)out,(int)(outwords+64),&cycles);
    if (rc) {printf("FAIL prepare rc=%d\n",rc);return 1;}
    double total=0,total_rpc=0,total_stage=0,total_core=0;
    for (int it=0;it<=reps;it++) {
        const int h=it&1;
        if (serial) memset(out,0xa5,outwords*4);
        if (!repack) pack(a[h],x,m);
        double all0=now_ms();
        if (repack||it==0) pack(a[h],x,m);
        memcpy(keys,keyref[h],keywords*4);
        double rpc0=now_ms();
        if (baseline) {
            rc=prl_fused_fused_run(m,n,flags,(byte *)x,(int)xs,(byte *)w,0,
                (int *)base_tt,(int)(ttwords+64),&cycles);
            if (!rc) {
                if (neon) cpu_digests_neon(base_tt,keyref[h],base_dig,m,n);
                else cpu_digests(base_tt,keyref[h],base_dig,m,n);
                if (mode==2) cpu_masks(base_dig,bound,base_mask,bands);
            }
        } else rc=prl_fused_fused_run_hash(m,n,flags,mode,(byte *)x,(int)xs,(byte *)w,0,
            &params,(int *)out,(int)(outwords+64),&cycles);
        double el=now_ms()-rpc0,all=now_ms()-all0;
        if (rc) {printf("FAIL hash rc=%d (0x%x)\n",rc,(unsigned)rc);return 1;}
        const uint32_t *expected=mode==1?digref[h]:maskref[h];
        const uint32_t *actual=baseline?(mode==1?base_dig:base_mask):out;
        for (size_t i=0;i<outwords;i++) if (actual[i]!=expected[i]) {
            printf("HASH_MISMATCH iter=%d word=%zu dsp=%08x ref=%08x\n",it,i,actual[i],expected[i]);return 2;
        }
        const uint32_t *diag=baseline?base_tt+ttwords:dg;
        if (it) {total+=all;total_rpc+=el;total_stage+=rpc0-all0;total_core+=(double)get64(diag,56)/19200.0;}
        printf("HASH iter=%d%s ms=%.4f core_ms=%.4f full_%s_MATCH hits=%zu\n",it,it?"":" warmup",all,
            (double)get64(diag,56)/19200.0,mode==1?"digests":"hit_masks",hits[h]);
    }
    printf("FUSED_HASH_PASSED avg_ms=%.4f operation_ms=%.4f input_stage_ms=%.4f DSP_core_ms=%.4f useful_GMAC_s=%.2f verification=all_%s baseline=%d ARM_hash=%s\n",
        total/reps,total_rpc/reps,total_stage/reps,total_core/reps,(double)m*n*2048*reps/total/1e6,mode==1?"digests":"hit_masks",baseline,neon?"official_NEON4":"official_portable");
    for (int h=0;h<2;h++) {free(a[h]);free(keyref[h]);free(ttref[h]);free(digref[h]);free(maskref[h]);}
    void *ptrs[]={x,w,out,base_tt};size_t sizes[]={xalloc,ws,outalloc,(ttwords+64)*4};
    for (unsigned k=0;k<4;k++) if (ptrs[k]) {unmap_persistent(ptrs[k],sizes[k]);rpcmem_free(ptrs[k]);}
    free(wj);free(base_dig);free(base_mask);return 0;
}

#endif /* PRL_HASH_HELPERS_ONLY */
