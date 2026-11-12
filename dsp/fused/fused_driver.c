#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sched.h>
#include <arm_neon.h>
#include "remote.h"
#include "rpcmem.h"
#include "fused.h"
#pragma weak remote_register_buf_attr2
#pragma weak remote_set_mode

static int g_persist_map;
static int map_persistent(void *ptr,size_t size) {
    if (!g_persist_map) return 0;
    if (!remote_register_buf_attr2) return 1;
    const int fd=rpcmem_to_fd(ptr);
    if (fd<0) return 1;
    remote_register_buf_attr2(ptr,size,-1,0);
    remote_register_buf_attr2(ptr,size,fd,FASTRPC_ATTR_KEEP_MAP|FASTRPC_ATTR_TRY_MAP_STATIC);
    return 0;
}
static void unmap_persistent(void *ptr,size_t size) {
    if (g_persist_map&&remote_register_buf_attr2) remote_register_buf_attr2(ptr,size,-1,0);
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1000000.0;
}
static uint64_t get64(const uint32_t *p,int at) { return p[at]|((uint64_t)p[at+1]<<32); }
static uint64_t rng(uint64_t *p) {
    uint64_t z=(*p+=UINT64_C(0x9e3779b97f4a7c15));
    z=(z^(z>>30))*UINT64_C(0xbf58476d1ce4e5b9);
    z=(z^(z>>27))*UINT64_C(0x94d049bb133111eb);
    return z^(z>>31);
}
static void pack_columns(const int8_t *a,uint8_t *x,int m) {
    for (int rb=0;rb<m/64;rb++) for (int s=0;s<16;s++)
        for (int kt=0;kt<4;kt++) for (int r=0;r<64;r++) {
            uint8x16_t z=vdupq_n_u8(128);
            const int8_t *src=a+((size_t)s*m+rb*64+r)*128+kt*32;
            uint8_t *dst=x+(((size_t)rb*16+s)*4+kt)*2048+r*32;
            vst1q_u8(dst,veorq_u8(vreinterpretq_u8_s8(vld1q_s8(src)),z));
            vst1q_u8(dst+16,veorq_u8(vreinterpretq_u8_s8(vld1q_s8(src+16)),z));
        }
}
static void pack_rows(const int8_t *a,uint8_t *x,int m) {
    const uint8x16_t z=vdupq_n_u8(128);
    for (int rb=0;rb<m/64;rb++) for (int s=0;s<16;s++) for (int r=0;r<64;r++) {
        const int8_t *src=a+((size_t)s*m+rb*64+r)*128;
        uint8_t *dst=x+((size_t)rb*16+s)*8192+r*32;
        for (int kt=0;kt<4;kt++) {
            vst1q_u8(dst+kt*2048,veorq_u8(vreinterpretq_u8_s8(vld1q_s8(src+kt*32)),z));
            vst1q_u8(dst+kt*2048+16,veorq_u8(vreinterpretq_u8_s8(vld1q_s8(src+kt*32+16)),z));
        }
    }
}
static int g_pack_rows,g_signed_rm;
static void pack(const int8_t *a,uint8_t *x,int m) {
    if (g_signed_rm) memcpy(x,a,(size_t)16*m*128);
    else if (g_pack_rows) pack_rows(a,x,m); else pack_columns(a,x,m);
}
static int32_t dot(const int8_t *a,const int8_t *b) {
    int32x4_t sum=vdupq_n_s32(0);
    for (int k=0;k<128;k+=16) {
        int8x16_t av=vld1q_s8(a+k),bv=vld1q_s8(b+k);
        sum=vpadalq_s16(sum,vmull_s8(vget_low_s8(av),vget_low_s8(bv)));
        sum=vpadalq_s16(sum,vmull_s8(vget_high_s8(av),vget_high_s8(bv)));
    }
    return vaddvq_s32(sum);
}
static int reference(const int8_t *a,const int8_t *wj,uint32_t *tt,int m,int n,int sample) {
    static const int off[16]={0,1,8,9,16,17,24,25,32,33,40,41,48,49,56,57};
    int32_t *cum=calloc(4096,4);
    if (!cum) return 1;
    for (int rb=0;rb<m/64;rb++) for (int band=0;band<n/64;band++) {
        if (sample && rb && band && rb!=m/64-1 && band!=n/64-1) continue;
        memset(cum,0,4096*4);
        for (int s=0;s<16;s++) {
            for (int r=0;r<64;r++) for (int c=0;c<64;c++)
                cum[r*64+c]+=dot(a+((size_t)s*m+rb*64+r)*128,
                                wj+((size_t)s*n+band*64+c)*128);
            for (int rbi=0;rbi<8;rbi++) for (int cb=0;cb<4;cb++) {
                uint32_t z=0;
                for (int u=0;u<8;u++) for (int v=0;v<16;v++) z^=(uint32_t)cum[(rbi+8*u)*64+cb*2+off[v]];
                tt[(((size_t)rb*(n/64)+band)*32+rbi*4+cb)*16+s]=z;
            }
        }
    }
    free(cum); return 0;
}
static int check(const uint32_t *o,const uint32_t *r,int m,int n,int sample) {
    for (int rb=0;rb<m/64;rb++) for (int band=0;band<n/64;band++) {
        if (sample && rb && band && rb!=m/64-1 && band!=n/64-1) continue;
        size_t base=((size_t)rb*(n/64)+band)*512;
        for (int i=0;i<512;i++) if (o[base+i]!=r[base+i]) {
            printf("MISMATCH rb=%d band=%d tile=%d slot=%d dsp=%08x ref=%08x\n",rb,band,i/16,i%16,o[base+i],r[base+i]);
            return 1;
        }
    }
    return 0;
}
#ifndef PRL_TRANSCRIPT_HELPERS_ONLY
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    const int m=argc>1?atoi(argv[1]):64,n=argc>2?atoi(argv[2]):64;
    const int reps=argc>3?atoi(argv[3]):10,flags=argc>4?atoi(argv[4]):2930;
    if (m<64||m>8192||n<64||n>2048||m%64||n%64||reps<1||reps>100000||flags<0||(flags&~65523)) return 2;
    const int maincpu=getenv("PRL_MAINCPU")?atoi(getenv("PRL_MAINCPU")):6;
    if (maincpu<0||maincpu>=CPU_SETSIZE) return 2;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(maincpu,&cs);
    if (sched_setaffinity(0,sizeof(cs),&cs)) { perror("sched_setaffinity"); return 1; }
    int full=getenv("PRL_FULL_INT8")?atoi(getenv("PRL_FULL_INT8")):0;
    const int sample=getenv("PRL_FUSED_SAMPLE")?atoi(getenv("PRL_FUSED_SAMPLE")):0;
    const int repack=getenv("PRL_REPACK")?atoi(getenv("PRL_REPACK")):0;
    g_signed_rm=(flags&8192)!=0;
    g_pack_rows=getenv("PRL_PACK_ROWS")?atoi(getenv("PRL_PACK_ROWS")):1;
    const char *pattern=getenv("PRL_PATTERN");
    if (!pattern) pattern="random";
    if (strcmp(pattern,"random")&&strcmp(pattern,"extreme")&&strcmp(pattern,"sparse")&&
        strcmp(pattern,"positive")&&strcmp(pattern,"negative")&&
        strcmp(pattern,"pulse")&&strcmp(pattern,"cancel")&&strcmp(pattern,"corner")&&strcmp(pattern,"signed_corner")) {
        fprintf(stderr,"invalid PRL_PATTERN\n"); return 2;
    }
    if (!strcmp(pattern,"corner")||!strcmp(pattern,"signed_corner")) full=1;
    uint64_t seed=getenv("PRL_SEED")?strtoull(getenv("PRL_SEED"),NULL,0):2026;
    const size_t xs=(size_t)16*m*128,ws=(size_t)16*n*128;
    const size_t words=(size_t)(m/64)*(n/64)*512;
    struct remote_rpc_control_unsigned_module req={.domain=3,.enable=1};
    int rc=remote_session_control(2,&req,sizeof(req));
    printf("unsigned rc=%d; fused m=%d n=%d reps=%d flags=%d seed=%llu range=%s pattern=%s reference=%s repack=%d maincpu=%d pack_rows=%d\n",rc,m,n,reps,flags,(unsigned long long)seed,full?"int8":"int7",pattern,sample?"edge bands sampled":"full",repack,maincpu,g_pack_rows);
    if (rc) return 1;
    const int serial=getenv("PRL_RPC_SERIAL")?atoi(getenv("PRL_RPC_SERIAL")):0;
    g_persist_map=getenv("PRL_PERSIST_MAP")?atoi(getenv("PRL_PERSIST_MAP")):(remote_register_buf_attr2!=NULL);
    if (serial && (!remote_set_mode||remote_set_mode(REMOTE_MODE_SERIAL))) {
        fprintf(stderr,"FAIL setting serial RPC mode\n");return 1;
    }
    printf("rpc serial=%d persistent_map_requested=%d (normal cache maintenance)\n",serial,g_persist_map);
    rpcmem_init();
    int8_t *a[2],*wj=malloc(ws);
    uint8_t *x[2]; uint32_t *ref[2];
    int8_t *w=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)ws);
    uint32_t *out=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)((words+64)*4));
    if (!w||!out||!wj) return 1;
    if (map_persistent(w,ws)||map_persistent(out,(words+64)*4)) return 1;
    for (int h=0;h<2;h++) {
        a[h]=malloc(xs); x[h]=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)xs);
        ref[h]=calloc(words,4);
        if (!a[h]||!x[h]||!ref[h]) return 1;
        if (map_persistent(x[h],xs)) return 1;
        for (size_t i=0;i<xs;i++) {
            unsigned v=(unsigned)(rng(&seed)>>33);
            int val=full?(int)(v&255):(int)(v&127)-64;
            if (!strcmp(pattern,"extreme")) val=(v&1)?(full?127:63):(full?-128:-64);
            if (!strcmp(pattern,"positive")) val=(v&1)?(full?127:63):0;
            if (!strcmp(pattern,"negative")) val=(v&1)?(full?-128:-64):0;
            if (!strcmp(pattern,"sparse")) val=0;
            a[h][i]=(int8_t)val;
        }
        if (!strcmp(pattern,"sparse")) {
            static const int rows[]={0,7,8,31,32,63};
            static const int ks[]={0,31,32,63,64,95,96,127};
            for (int s=0;s<16;s++) for (int rb=0;rb<m/64;rb++)
                for (unsigned q=0;q<sizeof(rows)/sizeof(rows[0]);q++)
                    a[h][((size_t)s*m+rb*64+rows[q])*128+ks[(s+q+h)%8]]=
                        (int8_t)(((s+q+h)&1)?(full?127:63):(full?-128:-64));
        }
        if (!strcmp(pattern,"pulse")||!strcmp(pattern,"cancel")) {
            /* Only slice 0 contributes; cancellation adds its exact negative at slice 15 with the same weights. */
            memset(a[h]+(size_t)m*128,0,xs-(size_t)m*128);
            for (size_t i=0;i<(size_t)m*128;i++) {
                int v=(int)a[h][i];
                if (v==-128) v=-127;
                a[h][i]=(int8_t)v;
                if (!strcmp(pattern,"cancel"))
                    a[h][(size_t)15*m*128+i]=(int8_t)-v;
            }
        }
        if (!strcmp(pattern,"corner")||!strcmp(pattern,"signed_corner")) {
            const int row=h?m-1:0;
            const int value=!strcmp(pattern,"signed_corner")?127:-128;
            memset(a[h],0,xs);
            for (int slice=0;slice<16;slice++)
                memset(a[h]+((size_t)slice*m+row)*128,(unsigned char)value,128);
        }
        double p0=now_ms(); pack(a[h],x[h],m); printf("pack input%d: %.3f ms\n",h,now_ms()-p0);
    }
    if (!g_signed_rm&&(g_pack_rows||getenv("PRL_PACK_BENCH"))) {
        uint8_t *check=malloc(xs);
        if (!check) return 1;
        for (int h=0;h<2;h++) {
            pack_columns(a[h],check,m); pack_rows(a[h],x[h],m);
            if (memcmp(check,x[h],xs)) { printf("FAIL packing equivalence\n"); return 2; }
        }
        free(check);
        printf("PACK_LAYOUT_PASSED all %zu bytes both inputs\n",xs*2);
        if (getenv("PRL_PACK_BENCH")) {
            double elapsed[2]={0,0};
            for (int i=0;i<400;i++) for (int flip=0;flip<2;flip++) {
                int mode=(i+flip)&1;
                double t0=now_ms();
                if (mode) pack_rows(a[i&1],x[i&1],m); else pack_columns(a[i&1],x[i&1],m);
                elapsed[mode]+=now_ms()-t0;
            }
            printf("PACK_BENCH_PASSED bytes=%zu reps=400 columns_ms=%.6f rows_ms=%.6f\n",xs,elapsed[0]/400,elapsed[1]/400);
            return 0;
        }
    }
    if (g_signed_rm) {
        if (memcmp(a[0],x[0],xs)||memcmp(a[1],x[1],xs)) return 2;
        printf("SIGNED_RM_INPUT_PASSED (ARM memcpy; signed-to-HMX conversion inside DSP)\n");
    }
    for (size_t i=0;i<ws;i++) {
        unsigned v=(unsigned)(rng(&seed)>>33);
        int val=full?(int)(v&255):(int)(v&127)-64;
        if (!strcmp(pattern,"extreme")||!strcmp(pattern,"sparse")) val=(v&1)?(full?127:63):(full?-128:-64);
        if (!strcmp(pattern,"positive")||!strcmp(pattern,"negative")) val=(v&1)?(full?127:63):0;
        w[i]=(int8_t)val;
    }
    if (!strcmp(pattern,"corner")||!strcmp(pattern,"signed_corner")) {
        memset(w,0,ws);
        for (int slice=0;slice<16;slice++) for (int k=0;k<128;k++) {
            const int column=!strcmp(pattern,"signed_corner")?n-1:0;
            w[((size_t)slice*128+k)*n+column]=-128;
        }
    }
    if (!strcmp(pattern,"cancel"))
        memcpy(w+(size_t)15*n*128,w,(size_t)n*128);
    for (int s=0;s<16;s++) for (int j=0;j<n;j++) for (int k=0;k<128;k++)
        wj[((size_t)s*n+j)*128+k]=w[((size_t)s*128+k)*n+j];
    double rt=now_ms();
    if (reference(a[0],wj,ref[0],m,n,sample)||reference(a[1],wj,ref[1],m,n,sample)) return 1;
    size_t reference_nonzero=0;
    for (int h=0;h<2;h++) for (size_t i=0;i<words;i++) reference_nonzero+=ref[h][i]!=0;
    printf("CPU reference ready: %.3f ms, nonzero_words=%zu/%zu\n",now_ms()-rt,reference_nonzero,words*2);
    if (!reference_nonzero) { fprintf(stderr,"FAIL degenerate zero reference\n"); return 2; }
    if (!strcmp(pattern,"pulse"))
        for (int h=0;h<2;h++) for (size_t i=0;i<words;i+=16)
            for (int s=1;s<16;s++) if (ref[h][i+s]!=ref[h][i]) return 2;
    if (!strcmp(pattern,"cancel"))
        for (int h=0;h<2;h++) for (size_t i=15;i<words;i+=16)
            if (ref[h][i]) return 2;

    uint64 cyc=0;
    if (serial) memset(out,0x5a,(words+64)*4);
    double prep0=now_ms();
    rc=prl_fused_fused_run(m,n,flags|4|8,(const byte *)x[0],(int)xs,(const byte *)w,(int)ws,(int *)out,(int)(words+64),&cyc);
    printf("prepare weights: rc=%d wall=%.3f ms DSP=%llu pcycles vtcm=%u\n",rc,now_ms()-prep0,(unsigned long long)get64(out+words,4),out[words+1]);
    if (out[words+21]>>16) printf("VTCM_LAYOUT_PREP compact=%u need=%u cfg=%u\n",
        (out[words+21]>>1)&1u,(out[words+21]>>16)*2048u,out[words+2]);
    if (rc) return 1;
    double total=0,total_inclusive=0,total_pack=0,best=1e30;
    uint64_t totcyc=0,totqtimer=0;
    for (int it=0;it<reps+1;it++) {
        const int h=it&1;
        /* PARALLEL mode requires output pages to be DSP-written/ARM-read-only. */
        if (serial) memset(out,0xa5,words*4);
        const double all0=now_ms();
        const int input_slot=repack?0:h;
        if (repack) pack(a[h],x[input_slot],m);
        double t0=now_ms();
        rc=prl_fused_fused_run(m,n,flags,(const byte *)x[input_slot],(int)xs,(const byte *)w,0,(int *)out,(int)(words+64),&cyc);
        double ms=now_ms()-t0;
        const double inclusive=now_ms()-all0;
        if (rc) {
            printf("FAIL run rc=%d (0x%x)\n",rc,(unsigned)rc);
            const uint32_t *d=out+words;
            printf("diagnostic workers=%u\n",d[20]);
            for (unsigned k=0;k<4;k++)
                printf("diagnostic worker=%u lock=%d status=%d unlock=%d processed=%u\n",
                    k,(int)d[40+k],(int)d[44+k],(int)d[48+k],d[52+k]);
            printf("diagnostic join_rc=%d,%d,%d,%d\n",(int)d[60],(int)d[61],(int)d[62],(int)d[63]);
            return 1;
        }
        if (out[words]!=0x46555344||!cyc||check(out,ref[h],m,n,sample)) return 2;
        printf("iter %d%s: %.3f ms %.1f GMAC/s transcripts MATCH pcycles=%llu\n",it,it?"":" warmup",ms,(double)m*n*2048/ms/1e6,(unsigned long long)cyc);
        if (flags&1) {
            const uint32_t *d=out+words;
            const uint64_t ticks=get64(d,56);
            if (ticks) printf("  DSP core=%.4f ms effective_pcycle_rate=%.1f MHz\n",
                (double)ticks/19200.0,(double)cyc*19.2/ticks);
            if (d[20]>0&&d[20]<=4)
                for (unsigned worker=0;worker<d[20];worker++)
                    printf("  worker %u busy=%llu producer_wait=%llu lock_rc=%d unlock_rc=%d\n",
                        worker,(unsigned long long)get64(d,22+2*worker),
                        (unsigned long long)get64(d,30+2*worker),(int)d[40+worker],(int)d[48+worker]);
            printf("  cycles prep=%llu stage=%llu mm=%llu read=%llu fold=%llu core=%llu total=%llu producer_wait=%llu\n",(unsigned long long)get64(d,4),(unsigned long long)get64(d,6),(unsigned long long)get64(d,8),(unsigned long long)get64(d,10),(unsigned long long)get64(d,12),(unsigned long long)get64(d,14),(unsigned long long)get64(d,16),(unsigned long long)get64(d,18));
        }
        if (it) { total+=ms;total_inclusive+=inclusive;total_pack+=t0-all0;totcyc+=cyc;totqtimer+=get64(out+words,56);if(ms<best)best=ms; }
    }
    printf("FUSED_PASSED avg=%.3f ms %.1f GMAC/s best=%.3f ms average DSP pcycles=%.0f verification=%s\n",total/reps,(double)m*n*2048*reps/total/1e6,best,(double)totcyc/reps,sample?"sampled":"full");
    if (totqtimer) printf("FUSED_DSP_CORE avg=%.4f ms effective_pcycle_rate=%.1f MHz\n",
        (double)totqtimer/reps/19200.0,(double)totcyc*19.2/totqtimer);
    if (out[words+21]>>16) printf("VTCM_LAYOUT_RUN compact=%u need=%u cfg=%u\n",
        (out[words+21]>>1)&1u,(out[words+21]>>16)*2048u,out[words+2]);
    if (out[words+38]==UINT32_C(0x53544147)) {
        const uint32_t *d=out+words;
        printf("ASYNC_STAGE blocks=%u lock_rc=%d status=%d unlock_rc=%d join_rc=%d\n",
            d[39],(int)d[42],(int)d[46],(int)d[50],(int)d[62]);
    }
    if ((flags&32768)&&!(flags&4096)) {
        const uint32_t *d=out+words;
        printf("ADAPTIVE_EXPORT short_per_rowblock=%u/%u removed_bytes=%llu per_call\n",
            d[38],d[39],(unsigned long long)(m/64)*d[38]*2048);
    }
    if (repack) printf("FUSED_WITH_PACK avg=%.3f ms %.1f GMAC/s pack=%.3f ms (input_mode=%s; excludes noise generation and final hash)\n",total_inclusive/reps,(double)m*n*2048*reps/total_inclusive/1e6,total_pack/reps,g_signed_rm?"signed_RM_memcpy":"ARM_pack");
    if (getenv("PRL_JOB_SWITCH")) {
        pack(a[0],x[0],m);
        /* Exercise same-shape new weights without explicit flag4, switching
         * retained/non-retained modes with a nonempty job each time. */
        int test_flags[]={2,0,18,50,114,306,370,2930,3442,3954,flags};
        for (unsigned job=0;job<sizeof(test_flags)/sizeof(test_flags[0]);job++) {
            for (size_t i=0;i<ws;i++) w[i]=(int8_t)(uint8_t)rng(&seed);
            for (int s=0;s<16;s++) for (int j=0;j<n;j++) for (int k=0;k<128;k++)
                wj[((size_t)s*n+j)*128+k]=w[((size_t)s*128+k)*n+j];
            if (reference(a[0],wj,ref[0],m,n,0)) return 1;
            int jf=test_flags[job];
            g_signed_rm=(jf&8192)!=0;
            pack(a[0],x[0],m);
            rc=prl_fused_fused_run(m,n,jf,(const byte *)x[0],(int)xs,
                (const byte *)w,(int)ws,(int *)out,(int)(words+64),&cyc);
            if (rc||check(out,ref[0],m,n,0)) { printf("FAIL job switch %u rc=%d\n",job,rc); return 2; }
            rc=prl_fused_fused_run(m,n,jf,(const byte *)x[0],(int)xs,
                (const byte *)w,0,(int *)out,(int)(words+64),&cyc);
            if (rc||check(out,ref[0],m,n,0)) return 2;
            printf("CACHE_JOB_SWITCH_PASSED job=%u flags=%d newW+cachedW verification=full\n",job,jf);
        }
        int cached_modes[]={114,306,370,50,18,306,2930,3442,3954,flags,50};
        for (unsigned pass=0;pass<sizeof(cached_modes)/sizeof(cached_modes[0]);pass++) {
            int jf=cached_modes[pass];
            g_signed_rm=(jf&8192)!=0;
            pack(a[0],x[0],m);
            rc=prl_fused_fused_run(m,n,jf,(const byte *)x[0],(int)xs,
                (const byte *)w,0,(int *)out,(int)(words+64),&cyc);
            if (rc||check(out,ref[0],m,n,0)) { printf("FAIL cached mode switch %u rc=%d\n",pass,rc); return 2; }
            printf("CACHE_MODE_SWITCH_PASSED pass=%u flags=%d verification=full\n",pass,jf);
        }
    }
    for (int h=0;h<2;h++) { free(a[h]);unmap_persistent(x[h],xs);rpcmem_free(x[h]);free(ref[h]); }
    free(wj);unmap_persistent(w,ws);unmap_persistent(out,(words+64)*4);rpcmem_free(w);rpcmem_free(out);rpcmem_deinit();
    return 0;
}

#endif /* PRL_TRANSCRIPT_HELPERS_ONLY */
