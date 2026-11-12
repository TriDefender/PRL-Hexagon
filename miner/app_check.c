/* Offline-only device check of the actual miner adapter's compact pipeline. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qnpu.h"

enum { M=2048, N=64, K=2048, ATTEMPTS=32, BATCHES=2 };
static const uint64_t NONCE_MASK = (UINT64_C(1)<<56)-1;

static void set_nonce(int8_t *a, uint64_t nonce) {
    for (unsigned i=0;i<8;i++) a[i]=(int8_t)((int)((nonce>>(7*i))&127)-64);
}

static int digest_at_most(const uint8_t digest[32], const uint8_t bound[32]) {
    for (int i=31;i>=0;i--) {
        if (digest[i]!=bound[i]) return digest[i]<bound[i];
    }
    return 1;
}

static int scalar_mask(pr_attempt_mats *am, const pr_commitment *commit,
                       const uint8_t bound[32], uint8_t expected[1024]) {
    pr_noise_operands(am,commit);
    int hits=0;
    for (unsigned rb=0;rb<32;rb++) for (unsigned lane=0;lane<32;lane++) {
        const uint32_t row=rb*64+lane/4, col=2*(lane%4);
        uint32_t transcript[16]; uint8_t digest[32];
        pr_tile_scalar(am->ap,am->btp,K,row,col,commit->a_noise_seed.b,transcript,digest);
        expected[rb*32+lane]=(uint8_t)digest_at_most(digest,bound);
        hits+=expected[rb*32+lane];
    }
    return hits;
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    char err[256];
    pr_qnpu_ctx *ctx=pr_qnpu_new(M,N,K,err,sizeof(err));
    if (!ctx) { fprintf(stderr,"APP_CHECK_FAIL init: %s\n",err); return 1; }
    pr_attempt_mats am={.m=M,.n=N,.k=K};
    am.a=malloc((size_t)M*K); am.bt=malloc((size_t)N*K);
    am.ap=malloc((size_t)M*K); am.btp=malloc((size_t)N*K);
    am.a16=malloc((size_t)M*K*sizeof(int16_t));
    am.bt16=malloc((size_t)N*K*sizeof(int16_t));
    int result=1;
    if (!am.a||!am.bt||!am.ap||!am.btp||!am.a16||!am.bt16) goto done;
    if (pr_qnpu_compound_attempts(ctx)!=ATTEMPTS) {
        fprintf(stderr,"APP_CHECK_FAIL compact batch unavailable\n"); goto done;
    }
    const uint64_t starts[2]={NONCE_MASK-15, 617};
    for (unsigned job=0;job<2;job++) {
        uint8_t header[76],bound[32];
        for (unsigned i=0;i<sizeof(header);i++) header[i]=(uint8_t)(i*17+job*53);
        memset(bound,0xff,sizeof(bound));
        if (!job) bound[31]=0x0f; /* mixed masks; second job forces all lanes */
        const uint64_t seed=job?2026:1;
        pr_fill_base(&am,seed);
        pr_qnpu_set_base_seed(ctx,seed);
        if (pr_qnpu_set_job(ctx,header,err,sizeof(err))) {
            fprintf(stderr,"APP_CHECK_FAIL set_job %u: %s\n",job,err); goto done;
        }
        pr_qnpu_set_bound(ctx,bound);
        for (unsigned batch=0;batch<BATCHES;batch++) {
            uint64_t first=(starts[job]+(uint64_t)batch*ATTEMPTS)&NONCE_MASK;
            if (pr_qnpu_compound_pipe(ctx,first)) {
                fprintf(stderr,"APP_CHECK_FAIL RPC job=%u batch=%u\n",job,batch); goto done;
            }
            for (unsigned a=0;a<ATTEMPTS;a++) {
                uint64_t nonce=UINT64_MAX,coords[PRL_QNPU_MAXHITS];
                const int hits=pr_qnpu_compound_result(ctx,a,&nonce,coords,NULL);
                const uint64_t expected_nonce=(first+a)&NONCE_MASK;
                if (hits<0||nonce!=expected_nonce) {
                    fprintf(stderr,"APP_CHECK_FAIL result job=%u batch=%u attempt=%u nonce=%" PRIu64 "\n",job,batch,a,nonce); goto done;
                }
                set_nonce(am.a,expected_nonce);
                pr_commitment commit;
                pr_commit(header,K,am.a,am.bt,M,N,&commit);
                if (memcmp(&commit,pr_qnpu_commitment(ctx),sizeof(commit))||
                    memcmp(am.a,pr_qnpu_host_a(ctx),(size_t)M*K)||
                    memcmp(am.bt,pr_qnpu_host_bt(ctx),(size_t)N*K)) {
                    fprintf(stderr,"APP_CHECK_FAIL commitment/base job=%u batch=%u attempt=%u\n",job,batch,a); goto done;
                }
                uint8_t actual[1024]={0};
                for (int i=0;i<hits;i++) {
                    uint32_t row=(uint32_t)coords[i],col=(uint32_t)(coords[i]>>32);
                    if (row>=M||row%64>=8||col>=8||col%2) {
                        fprintf(stderr,"APP_CHECK_FAIL coordinate\n"); goto done;
                    }
                    unsigned ix=(row/64)*32+(row%64)*4+col/2;
                    if (actual[ix]) {fprintf(stderr,"APP_CHECK_FAIL duplicate coordinate\n"); goto done;}
                    actual[ix]=1;
                }
                if (job==1&&hits!=1024) {
                    fprintf(stderr,"APP_CHECK_FAIL all-hit job=%u batch=%u attempt=%u hits=%d\n",job,batch,a,hits); goto done;
                }
                /* A complete scalar tile oracle for the first nonce of each
                 * job checks false negatives as well as returned hits. */
                if (!batch&&!a) {
                    uint8_t expected[1024];
                    int count=scalar_mask(&am,&commit,bound,expected);
                    if (hits!=count||memcmp(actual,expected,sizeof(actual))) {
                        fprintf(stderr,"APP_CHECK_FAIL scalar masks job=%u got=%d expected=%d\n",job,hits,count); goto done;
                    }
                    printf("APP_CHECK_SCALAR_PASSED job=%u nonce=%" PRIu64 " hits=%d tiles=1024\n",job,nonce,hits);
                }
            }
            printf("APP_CHECK_BATCH_PASSED job=%u batch=%u first=%" PRIu64 " attempts=32\n",job,batch,first);
        }
        (void)pr_qnpu_drain(ctx,NULL,NULL);
    }
    if (pr_qnpu_macs(ctx)!=(uint64_t)2*BATCHES*ATTEMPTS*M*N*K) {
        fprintf(stderr,"APP_CHECK_FAIL MAC accounting\n"); goto done;
    }
    printf("APP_CHECK_PASSED jobs=2 batches=4 attempts=128 nonce_wrap=1 scalar_full_attempts=2 network=none\n");
    result=0;
done:
    free(am.a); free(am.bt); free(am.ap); free(am.btp); free(am.a16); free(am.bt16);
    pr_qnpu_free(ctx);
    return result;
}
