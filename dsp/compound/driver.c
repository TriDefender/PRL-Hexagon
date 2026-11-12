/* Independent compound RPC driver: every attempt retains m=2048, its own nonce/commitment/key and packed 4MiB input. */
#define PRL_HASH_HELPERS_ONLY 1
#include "fused_hash_driver.c"
#include "noise_pack_neon.h"
#include "noise_compact_neon.h"
#include "pearl.h"
#include <pthread.h>
#include <errno.h>
#include "prep_pool.h"
#include "compound.h"
#include "protocol.h"

#ifndef PRL_BATCH_ATTEMPTS
#define PRL_BATCH_ATTEMPTS 4
#endif
#if PRL_BATCH_ATTEMPTS != 4 && PRL_BATCH_ATTEMPTS != 8 && PRL_BATCH_ATTEMPTS != 16 && PRL_BATCH_ATTEMPTS != 32
#error "PRL_BATCH_ATTEMPTS must be 4, 8, 16 or 32"
#endif
enum { BATCH_ATTEMPT_M=2048,BATCH_ATTEMPTS=PRL_BATCH_ATTEMPTS,
       BATCH_AGGREGATE_ROWS=BATCH_ATTEMPT_M*BATCH_ATTEMPTS,
       BATCH_PARAM_WORDS=BATCH_ATTEMPTS*8+8,BATCH_RING=3 };
enum { BATCH_FREE,BATCH_BUILDING,BATCH_READY,BATCH_RPC };
typedef struct {
    uint8_t *x;
    uint32_t *out;
    uint64_t tag,nonce[BATCH_ATTEMPTS];
    uint8_t nonce8[BATCH_ATTEMPTS][8];
    pr_commitment commit[BATCH_ATTEMPTS];
    int32_t *params; /* RPCMEM: count complete keys followed by one bound. */
    int state;
} batch_slot;
typedef struct {double seed0,noise0,ready1,rpc0,rpc1,done1,core_ms,dsp_total_ms;} batch_event;
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t changed;
    pthread_t worker;
    int worker_started,worker_ready,stop,failed;
    int n,flags,parallel,main_cpu,prep_cpu;
    unsigned preprocessor_quiet,prep_workers;
    int prep_cpus[PRL_PREP_MAX_WORKERS];
    prl_prep_pool *prep_pool;
    size_t xs_one,xs_rpc,ws,bands_one,bands_rpc,ttwords_one,digwords_one,digwords_rpc;
    int8_t *base_a,*base_bt,*w;
    uint8_t header[76];
    uint64_t nonce_start;
    uint32_t bound[8];
    pr_commitment job;
    pr_htree *tree;
    batch_slot slots[BATCH_RING];
    batch_event *events;
    uint64_t limit,consumed;
} batch_context;

static int batch_pin(int cpu) {
    if (cpu<0||cpu>=CPU_SETSIZE) return EINVAL;
    cpu_set_t cs;CPU_ZERO(&cs);CPU_SET(cpu,&cs);
    return sched_setaffinity(0,sizeof(cs),&cs)?errno:0;
}
static void batch_nonce8(uint64_t nonce,uint8_t bytes[8]) {
    for (unsigned j=0;j<8;j++) bytes[j]=(uint8_t)(int8_t)((int)((nonce>>(7*j))&127)-64);
}
static void batch_print_nonces(const batch_slot *s) {
    for (unsigned a=0;a<BATCH_ATTEMPTS;a++)
        printf("%s%llu",a?"/":"",(unsigned long long)s->nonce[a]);
}
static void batch_print_hits(const size_t hits[BATCH_ATTEMPTS]) {
    for (unsigned a=0;a<BATCH_ATTEMPTS;a++) printf("%s%zu",a?"/":"",hits[a]);
}
typedef struct {batch_context *context;batch_slot *slot;} batch_noise_job;
static int batch_noise_attempt(void *argument,unsigned a) {
    batch_noise_job *job=argument;
    batch_context *p=job->context;batch_slot *s=job->slot;
    /* Each independent call owns local factor/hash scratch and starts its protocol row index at zero. */
    return prl_noise_compact_record(s->commit[a].a_noise_seed.b,s->nonce8[a],
        s->x+PRL_COMPACT_BASE_BYTES+(size_t)a*PRL_COMPACT_RECORD_BYTES);
}
static int batch_prepare(batch_context *p,batch_slot *s,uint64_t tag,batch_event *e) {
    e->seed0=now_ms();s->tag=tag;
    memset(s->params,0,BATCH_PARAM_WORDS*sizeof(*s->params));
    memcpy(s->params+BATCH_ATTEMPTS*8,p->bound,32);
    for (unsigned a=0;a<BATCH_ATTEMPTS;a++) {
        s->nonce[a]=(p->nonce_start+tag*BATCH_ATTEMPTS+a)&UINT64_C(0x00ffffffffffffff);
        batch_nonce8(s->nonce[a],s->nonce8[a]);
        uint8_t first_chunk[1024];
        memcpy(first_chunk,p->base_a,sizeof(first_chunk));memcpy(first_chunk,s->nonce8[a],8);
        s->commit[a]=p->job;
        pr_htree_patch_chunk(p->tree,p->job.job_key.b,0,first_chunk);
        pr_htree_root(p->tree,s->commit[a].root_a.b);
        /* Never use aggregate rows in the protocol seed binding. */
        pr_bind_seeds(s->commit[a].root_a.b,s->commit[a].root_b.b,
                      BATCH_ATTEMPT_M,p->n,s->commit+a);
        memcpy(s->params+a*8,s->commit[a].a_noise_seed.b,32);
    }
    for (unsigned a=0;a<BATCH_ATTEMPTS;a++) for (unsigned b=a+1;b<BATCH_ATTEMPTS;b++)
        if (s->nonce[a]==s->nonce[b]||!memcmp(s->params+a*8,s->params+b*8,32)) return -1;
    e->noise0=now_ms();
    batch_noise_job job={p,s};
    const int noise_rc=prl_prep_pool_run(p->prep_pool,batch_noise_attempt,&job);
    if (noise_rc) return noise_rc; /* All helpers are already done writing. */
    e->ready1=now_ms();return 0;
}
static void *batch_preprocessor(void *arg) {
    batch_context *p=arg;const int pin_rc=batch_pin(p->prep_cpu);
    pthread_mutex_lock(&p->mu);
    if (pin_rc) {printf("FAIL preprocessing affinity errno=%d\n",pin_rc);p->failed=1;}
    p->worker_ready=1;pthread_cond_broadcast(&p->changed);
    uint64_t next=p->consumed;
    while (!p->stop&&!p->failed) {
        batch_slot *s=p->slots+next%BATCH_RING;
        while (!p->stop&&!p->failed&&(next>=p->limit||s->state!=BATCH_FREE))
            pthread_cond_wait(&p->changed,&p->mu);
        if (p->stop||p->failed) break;
        s->state=BATCH_BUILDING;pthread_mutex_unlock(&p->mu);
        const int rc=batch_prepare(p,s,next,p->events+next);
        pthread_mutex_lock(&p->mu);
        if (rc) {printf("FAIL batch preparation tag=%llu rc=%d\n",(unsigned long long)next,rc);p->failed=1;}
        else {s->state=BATCH_READY;next++;}
        pthread_cond_broadcast(&p->changed);
    }
    pthread_mutex_unlock(&p->mu);
    __atomic_store_n(&p->preprocessor_quiet,1,__ATOMIC_RELEASE);
    return NULL;
}
static int batch_preprocessor_finish(batch_context *p) {
    if (!p->worker_started) return 0;
    pthread_mutex_lock(&p->mu);p->stop=1;
    pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mu);
    const int rc=pthread_join(p->worker,NULL);
    if (rc) {
        while (!__atomic_load_n(&p->preprocessor_quiet,__ATOMIC_ACQUIRE)) sched_yield();
    } else p->worker_started=0;
    return rc;
}

/* Error RPC payloads may be stale; never promote such bytes to PASS. */
static int batch_summary_ok(const batch_context *p,const uint32_t *d,int mode,int rc,uint64 cycles,int expected_flags) {
    if (rc) {printf("FAIL compound RPC rc=%d payload_freshness_unproven=1\n",rc);return 0;}
    const unsigned chunks=(BATCH_ATTEMPTS+31)/32;
    const size_t words=mode==1?p->digwords_rpc:p->bands_rpc;
    if (!cycles||d[0]!=PRL_COMPOUND_MAGIC||d[1]!=PRL_COMPOUND_VERSION||
        d[2]!=BATCH_ATTEMPTS||d[3]!=(unsigned)p->n||d[4]!=(unsigned)expected_flags||d[5]!=(unsigned)mode||
        d[6]!=chunks||d[7]!=chunks||d[8]!=BATCH_ATTEMPTS||d[9]||d[10]||d[11]||d[12]||d[13]||
        d[14]!=1||d[15]||d[16]||!d[17]||d[18]||!d[19]||d[20]||!d[21]||d[22]||!d[23]||
        d[48]!=words||d[49]!=(mode==1?p->digwords_one:p->bands_one)||d[55]||d[62]!=32||
        d[CB_OUTER_PCYCLES]!=(uint32_t)cycles||!d[CB_CORE_TICKS]||!d[CB_OUTER_TICKS]) {
        printf("FAIL compound summary magic=%08x version=%u attempts=%u n=%u mode=%u chunks=%u/%u "
               "done_attempts=%u poison=%u core_rc=%u init_rc=%u begin_rc=%u end_rc=%u cycles=%llu\n",
            d[0],d[1],d[2],d[3],d[5],d[7],d[6],d[8],d[9],d[10],d[11],d[12],d[13],
            (unsigned long long)cycles);return 0;
    }
    return 1;
}

/* Raising the preparation limit after the timer starts prevents measured batches from being prepared during excluded warmup. */
static int batch_phase(batch_context *p,int calls,double *elapsed,size_t hits[BATCH_ATTEMPTS],
                    double *acquire_ms) {
    pthread_mutex_lock(&p->mu);
    const uint64_t first=p->consumed,target=first+(uint64_t)calls;
    const double start=now_ms();p->limit=target;pthread_cond_broadcast(&p->changed);
    pthread_mutex_unlock(&p->mu);
    memset(hits,0,sizeof(size_t)*BATCH_ATTEMPTS);*acquire_ms=0;
    for (uint64_t tag=first;tag<target;tag++) {
        batch_slot *s=p->slots+tag%BATCH_RING;const double wait0=now_ms();
        if (p->parallel) {
            pthread_mutex_lock(&p->mu);
            while (!p->failed&&s->state!=BATCH_READY) pthread_cond_wait(&p->changed,&p->mu);
            if (p->failed) {pthread_mutex_unlock(&p->mu);return 1;}
            if (s->tag!=tag||p->consumed!=tag) {
                printf("FAIL batch ownership expected=%llu actual=%llu\n",(unsigned long long)tag,(unsigned long long)s->tag);
                p->failed=2;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mu);return 2;
            }
            s->state=BATCH_RPC;pthread_mutex_unlock(&p->mu);
        } else {
            if (s->state!=BATCH_FREE||batch_prepare(p,s,tag,p->events+tag)) return 1;
            s->state=BATCH_RPC;
        }
        *acquire_ms+=now_ms()-wait0;
        batch_event *e=p->events+tag;uint64 cycles=0;e->rpc0=now_ms();
        const int rc=prl_compound_batch_run(BATCH_ATTEMPTS,p->n,p->flags,2,(byte *)s->x,(int)p->xs_rpc,
            (byte *)p->w,0,s->params,BATCH_PARAM_WORDS,(int *)s->out,(int)(p->bands_rpc+64),&cycles);
        e->rpc1=now_ms();const uint32_t *diag=s->out+p->bands_rpc;
        if (!batch_summary_ok(p,diag,2,rc,cycles,p->flags)) {
            printf("FAIL batch RPC tag=%llu nonces=",(unsigned long long)tag);
            batch_print_nonces(s);printf(" rc=%d cycles=%llu\n",rc,(unsigned long long)cycles);return 1;
        }
        e->core_ms=(double)diag[CB_CORE_TICKS]/19200.0;
        e->dsp_total_ms=(double)diag[CB_OUTER_TICKS]/19200.0;
        /* Each consecutive bands_one masks belong to exactly one nonce. */
        for (unsigned a=0;a<BATCH_ATTEMPTS;a++) for (size_t b=0;b<p->bands_one;b++)
            hits[a]+=(size_t)__builtin_popcount(s->out[a*p->bands_one+b]);
        e->done1=now_ms();
        pthread_mutex_lock(&p->mu);
        if (s->state!=BATCH_RPC||s->tag!=tag) {p->failed=2;pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mu);return 2;}
        s->state=BATCH_FREE;p->consumed=tag+1;
        pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mu);
    }
    *elapsed=now_ms()-start;return 0;
}
static double batch_overlap(const batch_event *e,int first,int calls) {
    int a=first,b=first;const int end=first+calls;double total=0;
    while (a<end&&b<end) {
        const double lo=e[a].seed0>e[b].rpc0?e[a].seed0:e[b].rpc0;
        const double hi=e[a].ready1<e[b].rpc1?e[a].ready1:e[b].rpc1;
        if (hi>lo) total+=hi-lo;
        if (e[a].ready1<e[b].rpc1) a++;else b++;
    }
    return total;
}

/* Full CPU reference work and both RPC modes are outside throughput timing. */
static int batch_verify(batch_context *p,int batches,pr_attempt_mats *am,
                     const int8_t *wj,uint32_t *digest_out) {
    uint32_t *tt=calloc(p->ttwords_one,4),*dig=malloc(p->digwords_rpc*4),*mask=malloc(p->bands_rpc*4);
    int8_t *a_sm=malloc(p->xs_one);int result=1;
    if (!tt||!dig||!mask||!a_sm) goto done;
    for (int v=0;v<batches;v++) {
        batch_slot *s=p->slots+v%BATCH_RING;
        if (batch_prepare(p,s,(uint64_t)v,p->events+v)) goto done;
        size_t expected_hits[BATCH_ATTEMPTS]={0};
        for (unsigned a=0;a<BATCH_ATTEMPTS;a++) {
            memcpy(am->a,p->base_a,p->xs_one);memcpy(am->a,s->nonce8[a],8);
            pr_commitment expected_commit;
            pr_commit(p->header,PRL_NOISE_K,am->a,am->bt,BATCH_ATTEMPT_M,p->n,&expected_commit);
            if (memcmp(&expected_commit,s->commit+a,sizeof(expected_commit))) {
                printf("FAIL batch commitment batch=%d attempt=%u nonce=%llu\n",v,a,(unsigned long long)s->nonce[a]);result=2;goto done;
            }
            pr_noise_operands(am,&expected_commit);
            const uint8_t *record=s->x+PRL_COMPACT_BASE_BYTES+(size_t)a*PRL_COMPACT_RECORD_BYTES;
            for (unsigned row=0;row<BATCH_ATTEMPT_M;row++) for (unsigned l=0;l<PRL_NOISE_K;l++) {
                const unsigned pi=record[PRL_COMPACT_P_OFFSET+l],qi=record[PRL_COMPACT_Q_OFFSET+l];
                if (pi>127||qi>127) {printf("FAIL compact index\n");result=2;goto done;}
                const int fp=(int)(int8_t)record[(size_t)row*128u+2u*(pi&63u)+(pi>>6)];
                const int fq=(int)(int8_t)record[(size_t)row*128u+2u*(qi&63u)+(qi>>6)];
                const int base=(row==0&&l<8)?(int)(int8_t)record[PRL_COMPACT_NONCE_OFFSET+l]:(int)p->base_a[(size_t)row*PRL_NOISE_K+l];
                const uint8_t got=(uint8_t)(base+fp-fq);
                if (got!=(uint8_t)am->ap[(size_t)row*PRL_NOISE_K+l]) {
                    printf("FAIL batch scalar A' batch=%d attempt=%u row=%u l=%u\n",v,a,row,l);result=2;goto done;
                }
            }
            for (unsigned slice=0;slice<16;slice++) for (unsigned row=0;row<BATCH_ATTEMPT_M;row++)
                memcpy(a_sm+((size_t)slice*BATCH_ATTEMPT_M+row)*128,
                    am->ap+(size_t)row*PRL_NOISE_K+slice*128,128);
            for (int slice=0;slice<16;slice++) for (int col=0;col<p->n;col++)
                if (memcmp(wj+((size_t)slice*p->n+col)*128,
                           am->btp+(size_t)col*PRL_NOISE_K+slice*128,128)) {
                    printf("FAIL batch cached B' changed\n");result=2;goto done;
                }
            if (reference(a_sm,wj,tt,BATCH_ATTEMPT_M,p->n,0)) goto done;
            cpu_digests(tt,(const uint32_t *)s->params+a*8,
                        dig+a*p->digwords_one,BATCH_ATTEMPT_M,p->n);
            expected_hits[a]=cpu_masks(dig+a*p->digwords_one,p->bound,
                                       mask+a*p->bands_one,p->bands_one);
        }
        uint64 cycles=0;
        int rc=prl_compound_batch_run(BATCH_ATTEMPTS,p->n,p->flags|4,1,(byte *)s->x,(int)p->xs_rpc,
            (byte *)p->w,(int)p->ws,s->params,BATCH_PARAM_WORDS,(int *)digest_out,(int)(p->digwords_rpc+64),&cycles);
        if (!batch_summary_ok(p,digest_out+p->digwords_rpc,1,rc,cycles,p->flags|4)||memcmp(digest_out,dig,p->digwords_rpc*4)) {
            printf("FAIL batch all digests batch=%d rc=%d\n",v,rc);result=2;goto done;
        }
        rc=prl_compound_batch_run(BATCH_ATTEMPTS,p->n,p->flags,2,(byte *)s->x,(int)p->xs_rpc,
            (byte *)p->w,0,s->params,BATCH_PARAM_WORDS,(int *)s->out,(int)(p->bands_rpc+64),&cycles);
        if (!batch_summary_ok(p,s->out+p->bands_rpc,2,rc,cycles,p->flags)||memcmp(s->out,mask,p->bands_rpc*4)) {
            printf("FAIL batch all masks batch=%d rc=%d\n",v,rc);result=2;goto done;
        }
        printf("BATCH_VERIFY batch=%d nonces=",v);batch_print_nonces(s);
        printf(" actual_m=%d aggregate_input_rows=%d distinct_keys=%d full_commitment+scalar_Aprime+cached_Bprime+CPU_TT+all_%zu_digests+all_%zu_masks_MATCH hits_by_attempt=",
            BATCH_ATTEMPT_M,BATCH_AGGREGATE_ROWS,BATCH_ATTEMPTS,p->bands_rpc*32,p->bands_rpc);
        batch_print_hits(expected_hits);printf("\n");
    }
    result=0;
done:
    free(tt);free(dig);free(mask);free(a_sm);return result;
}

static int pool_env_integer(const char *name,int fallback,int *out) {
    const char *value=getenv(name);
    if (!value) {*out=fallback;return 0;}
    char *end;errno=0;const long n=strtol(value,&end,10);
    if (errno||!value[0]||*end||n<0||n>=CPU_SETSIZE) return -1;
    *out=(int)n;return 0;
}
static int pool_configure(batch_context *p) {
    int workers;
    if (pool_env_integer("PRL_PREP_WORKERS",1,&workers)||
        (workers!=1&&workers!=2&&workers!=4&&workers!=6)||
        pool_env_integer("PRL_MAINCPU",workers>1?3:6,&p->main_cpu)||
        pool_env_integer("PRL_PREPCPU",7,&p->prep_cpu)||
        (p->parallel!=0&&p->parallel!=1)) return -1;
    p->prep_workers=(unsigned)workers;
    p->prep_cpus[0]=p->parallel?p->prep_cpu:p->main_cpu;
    const int defaults[5]={6,5,4,2,1};
    for (int id=1;id<workers;id++) p->prep_cpus[id]=defaults[id-1];
    const char *list=getenv("PRL_PREP_HELPER_CPUS");
    if (list) {
        if (workers==1) {if (*list) return -1;}
        else for (int id=1;id<workers;id++) {
            char *end;errno=0;const long cpu=strtol(list,&end,10);
            if (errno||end==list||cpu<0||cpu>=CPU_SETSIZE) return -1;
            p->prep_cpus[id]=(int)cpu;
            if (id+1<workers) {if (*end!=',') return -1;list=end+1;}
            else if (*end) return -1;
        }
    }
    if (p->parallel&&p->main_cpu==p->prep_cpu) return -1;
    for (int id=1;id<workers;id++) {
        if (p->prep_cpus[id]==p->main_cpu) return -1;
        for (int prior=0;prior<id;prior++)
            if (p->prep_cpus[id]==p->prep_cpus[prior]) return -1;
    }
    return 0;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    batch_context p={.mu=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
    /* count is attempts, never m. */
    p.n=argc>1?atoi(argv[1]):1024;
    const int calls=argc>2?atoi(argv[2]):(10000/BATCH_ATTEMPTS);p.flags=(argc>3?atoi(argv[3]):35698)|PRL_COMPACT_FLAG;
    p.parallel=getenv("PRL_HOT_PIPELINE")?atoi(getenv("PRL_HOT_PIPELINE")):1;
    if (pool_configure(&p)) {puts("FAIL invalid prep worker/CPU configuration");return 2;}
    const int warmup=getenv("PRL_HOT_WARMUP")?atoi(getenv("PRL_HOT_WARMUP")):(16/BATCH_ATTEMPTS);
    const int dump_events=getenv("PRL_HOT_EVENTS")?atoi(getenv("PRL_HOT_EVENTS")):0;
    const int input_uncached=getenv("PRL_INPUT_UNCACHED")?atoi(getenv("PRL_INPUT_UNCACHED")):0;
    const uint32_t requested_input_flags=input_uncached?RPCMEM_FLAG_UNCACHED:RPCMEM_DEFAULT_FLAGS;
    const int verify=getenv("PRL_VERIFY_BATCHES")?atoi(getenv("PRL_VERIFY_BATCHES")):(16/BATCH_ATTEMPTS);
    p.nonce_start=getenv("PRL_NONCE_START")?strtoull(getenv("PRL_NONCE_START"),NULL,0):124;
    const uint64_t base_seed=getenv("PRL_SEED")?strtoull(getenv("PRL_SEED"),NULL,0):2026;
    const int required=2|32|64|256|2048;
    if (p.n<64||p.n>2048||p.n%64||calls<1||calls>100000||
        verify<0||verify>500||(verify&&verify*BATCH_ATTEMPTS<16)||warmup<1||warmup>10000||p.flags<0||(p.flags&~(65523|PRL_COMPACT_FLAG))||
        (p.flags&required)!=required||!(p.flags&1536)||(p.flags&8192)||
        (p.parallel&&p.main_cpu==p.prep_cpu)||p.nonce_start>UINT64_C(0x00ffffffffffffff)) return 2;
    int result=1;int8_t *wj=NULL;uint32_t *digest_out=NULL;
    pr_attempt_mats am={.m=BATCH_ATTEMPT_M,.n=(uint32_t)p.n,.k=PRL_NOISE_K};
    p.xs_one=(size_t)BATCH_ATTEMPT_M*PRL_NOISE_K;p.xs_rpc=PRL_COMPACT_TOTAL_BYTES(BATCH_ATTEMPTS);
    p.ws=(size_t)p.n*PRL_NOISE_K;p.bands_one=(size_t)(BATCH_ATTEMPT_M/64)*(p.n/64);
    p.bands_rpc=p.bands_one*BATCH_ATTEMPTS;p.ttwords_one=p.bands_one*512;
    p.digwords_one=p.bands_one*256;p.digwords_rpc=p.digwords_one*BATCH_ATTEMPTS;
    if (batch_pin(p.main_cpu)) {perror("main affinity");return 1;}
    struct remote_rpc_control_unsigned_module req={.domain=3,.enable=1};
    int rc=remote_session_control(2,&req,sizeof(req));
    if (rc) {printf("FAIL unsigned rc=%d\n",rc);return 1;}
    const int serial=getenv("PRL_RPC_SERIAL")?atoi(getenv("PRL_RPC_SERIAL")):0;
    if (serial&&(!remote_set_mode||remote_set_mode(REMOTE_MODE_SERIAL))) return 1;
    g_persist_map=getenv("PRL_PERSIST_MAP")?atoi(getenv("PRL_PERSIST_MAP")):(remote_register_buf_attr2!=NULL);
    rpcmem_init();const double job0=now_ms();
    p.base_a=malloc(p.xs_one);p.base_bt=malloc(p.ws);
    p.w=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)p.ws);
    p.events=calloc((size_t)verify+warmup+calls,sizeof(*p.events));
    am.a=malloc(p.xs_one);am.bt=p.base_bt;am.ap=malloc(p.xs_one);am.btp=malloc(p.ws);
    am.a16=malloc(p.xs_one*2);am.bt16=malloc(p.ws*2);wj=malloc(p.ws);
    if (!p.base_a||!p.base_bt||!p.w||!p.events||!am.a||!am.ap||!am.btp||!am.a16||!am.bt16||!wj) goto cleanup;
    if (map_persistent(p.w,p.ws)) goto cleanup;
    for (int k=0;k<BATCH_RING;k++) {
        p.slots[k].x=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,requested_input_flags,(int)p.xs_rpc);
        p.slots[k].params=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,BATCH_PARAM_WORDS*4);
        p.slots[k].out=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)((p.bands_rpc+64)*4));
        if (!p.slots[k].x||!p.slots[k].out||!p.slots[k].params||map_persistent(p.slots[k].x,p.xs_rpc)||
            map_persistent(p.slots[k].params,BATCH_PARAM_WORDS*4)||
            map_persistent(p.slots[k].out,(p.bands_rpc+64)*4)) goto cleanup;
    }
    pr_attempt_mats base={.m=BATCH_ATTEMPT_M,.n=(uint32_t)p.n,.k=PRL_NOISE_K,.a=p.base_a,.bt=p.base_bt};
    pr_fill_base(&base,base_seed);
    for (int k=0;k<BATCH_RING;k++) memcpy(p.slots[k].x,p.base_a,p.xs_one);
    uint64_t header_rng=20261108;
    for (unsigned k=0;k<76;k++) p.header[k]=(uint8_t)rng(&header_rng);
    store_le32(p.header,3);store_le32(p.header+68,1700000000);store_le32(p.header+72,0x1d00ffff);
    const char *header_hex=getenv("PRL_HEADER76");size_t decoded=0;
    if (header_hex&&(pr_hex_decode(p.header,76,header_hex,&decoded)||decoded!=76)) {printf("FAIL header hex\n");result=2;goto cleanup;}
    uint8_t target[32];const char *target_hex=getenv("PRL_TARGET_BE");
    if (!target_hex) target_hex="00000000ffff0000000000000000000000000000000000000000000000000000";
    pr_u256 bound;
    if (pr_hex_decode(target,32,target_hex,&decoded)||decoded!=32||
        !pr_share_bound(target,PRL_NOISE_K,&bound)) {printf("FAIL share target\n");result=2;goto cleanup;}
    memcpy(p.bound,bound.w,32);
    pr_commit(p.header,PRL_NOISE_K,p.base_a,p.base_bt,BATCH_ATTEMPT_M,p.n,&p.job);
    p.tree=pr_htree_new(p.job.job_key.b,(uint8_t *)p.base_a,p.xs_one);if (!p.tree) goto cleanup;
    memcpy(am.a,p.base_a,p.xs_one);pr_noise_operands(&am,&p.job);
    for (int s=0;s<16;s++) for (int j=0;j<p.n;j++) for (int k=0;k<128;k++) {
        const int8_t value=am.btp[(size_t)j*PRL_NOISE_K+s*128+k];
        p.w[((size_t)s*128+k)*p.n+j]=value;wj[((size_t)s*p.n+j)*128+k]=value;
    }
    rc=prl_prep_pool_start(&p.prep_pool,p.prep_workers,p.prep_cpus,batch_pin);
    if (rc) {printf("FAIL prep pool startup rc=%d\n",rc);goto cleanup;}
    uint64 cycles=0; /* Weight cache is initialized by the first reference RPC. */
    char key_hex[65],bseed_hex[65];pr_hex_encode(key_hex,p.job.job_key.b,32);pr_hex_encode(bseed_hex,p.job.b_noise_seed.b,32);
    printf("BATCH config runner=compound_batch actual_m=%d aggregate_input_rows=%d attempts_per_call=%d n=%d k=2048 calls=%d verify_batches=%d warmup_calls=%d flags=%d pipeline=%d maincpu=%d prepcpu=%d ring=%d nonce_encoding=8x7bit_minus64 persist=%d rpc_serial=%d dump_events=%d requested_input_flags=%u\n",
        BATCH_ATTEMPT_M,BATCH_AGGREGATE_ROWS,BATCH_ATTEMPTS,p.n,calls,verify,warmup,p.flags,
        p.parallel,p.main_cpu,p.prep_cpu,BATCH_RING,g_persist_map,serial,dump_events,(unsigned)requested_input_flags);
    printf("BATCH_PREP_POOL workers=%u coordinator_cpu=%d helpers=",p.prep_workers,p.prep_cpus[0]);
    for (unsigned id=1;id<p.prep_workers;id++) printf("%s%d",id==1?"":"/",p.prep_cpus[id]);
    puts(" assignment=attempt_mod_workers worker0=coordinator validation_worker0=main_thread");
    printf("BATCH job_setup_with_oracle_ms=%.4f job_key=%s b_noise_seed=%s Bprime_reused_per_job=1 target_BE=%s normal_cache_maintenance=1\n",
        now_ms()-job0,key_hex,bseed_hex,target_hex);
    if (verify) {
        digest_out=rpcmem_alloc(RPCMEM_DEFAULT_HEAP,RPCMEM_DEFAULT_FLAGS,(int)((p.digwords_rpc+64)*4));
        if (!digest_out||map_persistent(digest_out,(p.digwords_rpc+64)*4)) goto cleanup;
        const double validation0=now_ms();result=batch_verify(&p,verify,&am,wj,digest_out);if (result) goto cleanup;
        printf("BATCH_REFERENCE_PASSED full_batches=%d distinct_nonces=%d actual_m=%d aggregate_input_rows=%d full_scope=independent_commitment+scalar_noise+CPU_TT+official_BLAKE3+both_RPC_modes elapsed_ms=%.3f excluded_from_throughput=1\n",
            verify,verify*BATCH_ATTEMPTS,BATCH_ATTEMPT_M,BATCH_AGGREGATE_ROWS,now_ms()-validation0);
    }
    free(am.a);am.a=NULL;free(am.ap);am.ap=NULL;free(am.btp);am.btp=NULL;
    free(am.a16);am.a16=NULL;free(am.bt16);am.bt16=NULL;free(wj);wj=NULL;
    if (digest_out) {unmap_persistent(digest_out,(p.digwords_rpc+64)*4);rpcmem_free(digest_out);digest_out=NULL;}
    p.consumed=(uint64_t)verify;p.limit=p.consumed;
    if (p.parallel) {
        rc=pthread_create(&p.worker,NULL,batch_preprocessor,&p);
        if (rc) {printf("FAIL pthread_create rc=%d\n",rc);result=1;goto cleanup;}
        p.worker_started=1;pthread_mutex_lock(&p.mu);
        while (!p.worker_ready) pthread_cond_wait(&p.changed,&p.mu);
        const int failed=p.failed;pthread_mutex_unlock(&p.mu);if (failed) {result=1;goto cleanup;}
    }
    double warmup_ms=0,total_ms=0,acquire_ms=0;size_t hits[BATCH_ATTEMPTS]={0};
    result=batch_phase(&p,warmup,&warmup_ms,hits,&acquire_ms);if (result) goto cleanup;
    printf("BATCH warmup_drained calls=%d distinct_nonces=%d elapsed_ms=%.4f excluded_from_measurement=1\n",warmup,warmup*BATCH_ATTEMPTS,warmup_ms);
    result=batch_phase(&p,calls,&total_ms,hits,&acquire_ms);if (result) goto cleanup;
    const int first=verify+warmup;double seeds=0,noise=0,rpc=0,core=0,dsp_total=0,consume=0;
    for (int j=first;j<first+calls;j++) {
        const batch_event *e=p.events+j;seeds+=e->noise0-e->seed0;noise+=e->ready1-e->noise0;
        rpc+=e->rpc1-e->rpc0;core+=e->core_ms;dsp_total+=e->dsp_total_ms;consume+=e->done1-e->rpc1;
    }
    const double attempts=(double)calls*BATCH_ATTEMPTS;
    const double overlap=batch_overlap(p.events,first,calls);
    printf("MINING_COMPOUND_BATCH_%s attempts_per_call=%d call_ms=%.4f attempt_ms=%.4f total_ms=%.4f calls=%d distinct_attempts=%d useful_GMAC_s=%.2f seed_ms_per_call=%.4f NEON_noise_pack_ms_per_call=%.4f RPC_ms_per_call=%.4f DSP_core_sum_ms_per_call=%.4f consume_ms_per_call=%.4f input_acquire_ms_per_call=%.4f hits_by_attempt=",
        verify?"PASSED":"COMPLETED",BATCH_ATTEMPTS,total_ms/calls,total_ms/attempts,total_ms,calls,calls*BATCH_ATTEMPTS,
        (double)BATCH_ATTEMPT_M*p.n*PRL_NOISE_K*attempts/total_ms/1e6,
        seeds/calls,noise/calls,rpc/calls,core/calls,consume/calls,acquire_ms/calls);
    batch_print_hits(hits);printf("\n");
    printf("BATCH_MAC actual_MAC_per_attempt=%llu total_actual_MAC=%llu counted_attempts=%d\n",
        (unsigned long long)((uint64_t)BATCH_ATTEMPT_M*p.n*PRL_NOISE_K),
        (unsigned long long)((uint64_t)BATCH_ATTEMPT_M*p.n*PRL_NOISE_K*calls*BATCH_ATTEMPTS),calls*BATCH_ATTEMPTS);
    printf("BATCH_TIMING attempts_per_call=%d DSP_outer_including_clock_API_ms_per_call=%.4f DSP_outer_including_clock_API_minus_core_ms_per_call=%.4f RPC_minus_DSP_outer_including_clock_API_ms_per_call=%.4f\n",
        BATCH_ATTEMPTS,dsp_total/calls,(dsp_total-core)/calls,(rpc-dsp_total)/calls);
    printf("BATCH_SCOPE actual_m=%d aggregate_input_rows=%d pipeline=%d verified_prefix_batches=%d verified_prefix_attempts=%d measured_nonces=%llu..%llu measured_full_reference=0 distinct_key_groups=%d preprocessing_with_RPC_ms=%.4f fraction_preprocessing_overlapped=%.4f includes=real_nonce_commitments+real_keys+real_Aprime_direct_pack+one_RPC+DSP_hash+per_attempt_mask_consume excludes=job_setup+validation+network+proof_submission\n",
        BATCH_ATTEMPT_M,BATCH_AGGREGATE_ROWS,p.parallel,verify,verify*BATCH_ATTEMPTS,
        (unsigned long long)((p.nonce_start+(uint64_t)first*BATCH_ATTEMPTS)&UINT64_C(0x00ffffffffffffff)),
        (unsigned long long)((p.nonce_start+(uint64_t)(first+calls)*BATCH_ATTEMPTS-1)&UINT64_C(0x00ffffffffffffff)),
        BATCH_ATTEMPTS,overlap,seeds+noise?overlap/(seeds+noise):0);
    const uint64_t last_tag=(uint64_t)first+(uint64_t)calls-1;
    const uint32_t *last_diag=p.slots[last_tag%BATCH_RING].out+p.bands_rpc;
    printf("BATCH_LAST_KERNEL_PHASE_PCY sample=last_RPC stage=%u mm=%u read_and_publish=%u consumer_busy=%u queue_acquire_wait=%u tile_loop_estimate=%u thread_start_finish=%u core=%u\n",
        last_diag[CB_SUM_STAGE_PCY],last_diag[CB_SUM_MM_PCY],
        last_diag[CB_SUM_READ_PCY],last_diag[CB_SUM_FOLD_PCY],
        last_diag[CB_SUM_QUEUE_ACQUIRE_WAIT_PCY],
        last_diag[CB_SUM_TILE_LOOP_PCY],
        last_diag[CB_SUM_LIFECYCLE_PCY],
        last_diag[CB_SUM_CORE_PCYCLES]);
    printf("BATCH_CLOCK sample=last_RPC_boundary_only tag=%llu policy=%u "
           "pre_Q6_rc=%d pre_Q6_Hz=%u pre_HMX_rc=%d pre_HMX_Hz=%u "
           "post_Q6_rc=%d post_Q6_Hz=%u post_HMX_rc=%d post_HMX_Hz=%u "
           "queries_while_vote_active=1 full_call_residency_unproven=1\n",
        (unsigned long long)last_tag,last_diag[CB_POLICY],
        (int32_t)last_diag[CB_PRE_Q6_RC],last_diag[CB_PRE_Q6_HZ],
        (int32_t)last_diag[CB_PRE_HMX_RC],last_diag[CB_PRE_HMX_HZ],
        (int32_t)last_diag[CB_POST_Q6_RC],last_diag[CB_POST_Q6_HZ],
        (int32_t)last_diag[CB_POST_HMX_RC],last_diag[CB_POST_HMX_HZ]);
    /* This adds no clocks, formatting, or I/O to the timed loop. */
    if (dump_events) for (int j=first;j<first+calls;j++) {
        const batch_event *e=p.events+j;
        const uint64_t nonce_first=(p.nonce_start+(uint64_t)j*BATCH_ATTEMPTS)&UINT64_C(0x00ffffffffffffff);
        const uint64_t nonce_last=(p.nonce_start+(uint64_t)(j+1)*BATCH_ATTEMPTS-1)&UINT64_C(0x00ffffffffffffff);
        printf("HOT_EVENT call=%d tag=%d attempts=%d nonce_first=%llu nonce_last=%llu seed_ms=%.4f noise_ms=%.4f RPC_ms=%.4f core_ms=%.4f DSP_outer_including_clock_API_ms=%.4f\n",
            j-first,j,BATCH_ATTEMPTS,(unsigned long long)nonce_first,(unsigned long long)nonce_last,
            e->noise0-e->seed0,e->ready1-e->noise0,e->rpc1-e->rpc0,e->core_ms,e->dsp_total_ms);
    }
    result=0;
cleanup:
    {
        const int join_rc=batch_preprocessor_finish(&p);
        if (join_rc) {printf("FAIL preprocessor join rc=%d externally_quiet=1\n",join_rc);result=1;}
    }
    {
        const int pool_rc=prl_prep_pool_finish(&p.prep_pool);
        if (pool_rc) {printf("FAIL prep pool finish rc=%d metadata_retained=1 externally_quiet=1\n",pool_rc);result=1;}
    }
    if (digest_out) {unmap_persistent(digest_out,(p.digwords_rpc+64)*4);rpcmem_free(digest_out);}
    for (int k=0;k<BATCH_RING;k++) {
        if (p.slots[k].x) {unmap_persistent(p.slots[k].x,p.xs_rpc);rpcmem_free(p.slots[k].x);}
        if (p.slots[k].params) {unmap_persistent(p.slots[k].params,BATCH_PARAM_WORDS*4);rpcmem_free(p.slots[k].params);}
        if (p.slots[k].out) {unmap_persistent(p.slots[k].out,(p.bands_rpc+64)*4);rpcmem_free(p.slots[k].out);}
    }
    if (p.w) {unmap_persistent(p.w,p.ws);rpcmem_free(p.w);}
    pr_htree_free(p.tree);free(p.base_a);free(p.base_bt);free(p.events);
    free(am.a);free(am.ap);free(am.btp);free(am.a16);free(am.bt16);free(wj);
    rpcmem_deinit();pthread_cond_destroy(&p.changed);pthread_mutex_destroy(&p.mu);return result;
}
