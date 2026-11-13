/*
 * main.c — prl-miner: a CPU "pickaxe" for Pearl (PRL).
 *
 *   prl-miner --selftest miner/tests/golden.txt   bit-exact checks vs official vectors
 *   prl-miner --bench [--shape 256,256,2048] [-t N]
 *   prl-miner -o br.pearl.herominers.com:1200 -u prl1... -w rig1 [options]
 *   prl-miner --job <header76hex>,<target64hex> --seed 1   one offline attempt
 *
 * Options:
 *   -o, --pool URL        host:port ("tls://" prefix or --tls for TLS pools)
 *   -u, --wallet ADDR     PRL taproot address (prl1...)
 *   -w, --worker NAME     worker name (default "cpu")
 *       --dialect NAME    object | kryptex   (default object)
 *       --agent STR       user agent (default prl-c-miner/<ver>)
 *   -t, --threads N       mining threads (default: nproc)
 *       --shape M,N,K     attempt shape (default 256,256,2048; M,N,K multiples of 64)
 *       --seed N          first attempt seed (default: time)
 *       --job A,B         offline: header76 hex, target 64-hex (big-endian)
 *       --selftest FILE   run the bit-exact selftest against the golden file
 *       --bench SECONDS   mine against a synthetic never-hitting job and report rate
 *       --tls             force TLS (needs an OpenSSL build)
 *   -v                    verbose
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "blake3/blake3.h"
#include "pearl.h"
#include "stratum.h"

#ifdef WITH_GPU
#include "gpu.h"
#endif

#ifdef WITH_IGPU
#include "igpu.h"
#endif

#ifdef WITH_QNPU
#include "qnpu.h"
#endif

#define MINER_VERSION "0.1.0"
#define MINER_AGENT "prl-c-miner/" MINER_VERSION

#ifdef WITH_GPU
static int g_use_gpu;
#endif
#ifdef WITH_IGPU
static int g_use_igpu;
#endif
#ifdef WITH_QNPU
static int g_use_qnpu;
#endif

static void now_str(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%H:%M:%S", &tm);
}

#define LOGI(...)                                                    \
    do {                                                             \
        char ts_[16];                                                \
        now_str(ts_, sizeof ts_);                                    \
        printf("[%s] ", ts_);                                        \
        printf(__VA_ARGS__);                                         \
        putchar('\n');                                               \
        fflush(stdout);                                              \
    } while (0)

static unsigned pool_stats_interval_seconds(void) {
    const char *setting = getenv("PRL_POOL_STATS_INTERVAL");
    if (!setting || !*setting) return 15;
    char *end = NULL;
    unsigned long seconds = strtoul(setting, &end, 10);
    if (end == setting || *end || seconds < 1 || seconds > 60) return 15;
    return (unsigned)seconds;
}

static const char USAGE[] =
    "usage: prl-miner [MODE] [options]\n"
    "modes: (default) mine on a pool | --selftest FILE | --bench SECONDS | --job A,B\n"
    "  -o, --pool URL      pool host:port (tls:// prefix or --tls)\n"
    "  -u, --wallet ADDR   PRL taproot address (prl1...)\n"
    "  -w, --worker NAME   worker name (default cpu)\n"
    "      --dialect NAME  object (HeroMiners/LuckyPool) | kryptex\n"
    "      --agent STR     agent string\n"
    "  -t, --threads N     mining threads (default nproc)\n"
    "      --shape M,N,K   attempt shape, multiples of 64 (default 256,256,2048)\n"
    "      --seed N        first attempt seed\n"
    "      --job A,B       offline: header76-hex,target-64hex (one attempt per thread)\n"
    "      --selftest F    bit-exact selftest against the golden file\n"
    "      --bench S       benchmark for S seconds (synthetic job)\n"
    "      --tls           use TLS (OpenSSL build only)\n"
#ifdef WITH_GPU
    "      --gpu           mine on the NVIDIA GPU (CUDA build)\n"
#endif
#ifdef WITH_IGPU
    "      --igpu          mine on the AMD iGPU (OpenCL build)\n"
#endif
#ifdef WITH_QNPU
    "      --npu           mine on the Qualcomm NPU (QNN HTP build, Android)\n"
#endif
    "  -v                  verbose\n";

/* ============================ selftest =================================== */

static int fail_count;
#define CHECK(cond, name)                                   \
    do {                                                    \
        if (cond) printf("  ok   %s\n", name);              \
        else { printf("  FAIL %s\n", name); fail_count++; } \
    } while (0)

typedef struct {
    uint32_t m, n, k;
    uint64_t seed;
    char header76[160], config52[160], job_key[80], root_a[80], root_b[80], bound_a[80], bound_b[80],
        b_seed[80], a_seed[80], a_row0[200], bt_row0[200], ap_row0[200], btp_row0[200], al_row0[300],
        pairs_a[128], share_b64[230000];
    struct { uint32_t tr, tc, words[16]; char digest[80]; } tiles[8];
    int ntiles;
} golden_case;

static int golden_load(const char *path, golden_case *cases, int cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *line = malloc(300000);
    if (!line) abort();
    int n = -1;
    while (fgets(line, 300000, f)) {
        if (strncmp(line, "=== case", 8) == 0) {
            n++;
            if (n >= cap) break;
            memset(&cases[n], 0, sizeof(golden_case));
            continue;
        }
        if (n < 0) continue;
        golden_case *c = &cases[n];
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *v = sp + 1;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r')) v[--vl] = 0;
        if (!strcmp(line, "M")) c->m = (uint32_t)strtoul(v, 0, 10);
        else if (!strcmp(line, "N")) c->n = (uint32_t)strtoul(v, 0, 10);
        else if (!strcmp(line, "K")) c->k = (uint32_t)strtoul(v, 0, 10);
        else if (!strcmp(line, "SEED")) c->seed = strtoull(v, 0, 10);
        else if (!strcmp(line, "HEADER76")) snprintf(c->header76, 160, "%s", v);
        else if (!strcmp(line, "CONFIG52")) snprintf(c->config52, 160, "%s", v);
        else if (!strcmp(line, "JOB_KEY")) snprintf(c->job_key, 80, "%s", v);
        else if (!strcmp(line, "ROOT_A")) snprintf(c->root_a, 80, "%s", v);
        else if (!strcmp(line, "ROOT_B")) snprintf(c->root_b, 80, "%s", v);
        else if (!strcmp(line, "BOUND_A")) snprintf(c->bound_a, 80, "%s", v);
        else if (!strcmp(line, "BOUND_B")) snprintf(c->bound_b, 80, "%s", v);
        else if (!strcmp(line, "B_SEED")) snprintf(c->b_seed, 80, "%s", v);
        else if (!strcmp(line, "A_SEED")) snprintf(c->a_seed, 80, "%s", v);
        else if (!strcmp(line, "A_ROW0")) snprintf(c->a_row0, 200, "%s", v);
        else if (!strcmp(line, "BT_ROW0")) snprintf(c->bt_row0, 200, "%s", v);
        else if (!strcmp(line, "AP_ROW0")) snprintf(c->ap_row0, 200, "%s", v);
        else if (!strcmp(line, "BTP_ROW0")) snprintf(c->btp_row0, 200, "%s", v);
        else if (!strcmp(line, "AL_ROW0")) snprintf(c->al_row0, 300, "%s", v);
        else if (!strcmp(line, "PAIRS_A")) snprintf(c->pairs_a, 128, "%s", v);
        else if (!strcmp(line, "TILE") && c->ntiles < 8) {
            unsigned tr, tc;
            char words[200], digest[80];
            if (sscanf(v, "%u %u %199s %79s", &tr, &tc, words, digest) == 4) {
                c->tiles[c->ntiles].tr = tr;
                c->tiles[c->ntiles].tc = tc;
                /* words were printed as 16 big-endian u32 hex groups */
                for (int w = 0; w < 16; w++) {
                    char tmp[9];
                    memcpy(tmp, words + 8 * w, 8);
                    tmp[8] = 0;
                    c->tiles[c->ntiles].words[w] = (uint32_t)strtoul(tmp, 0, 16);
                }
                snprintf(c->tiles[c->ntiles].digest, 80, "%s", digest);
                c->ntiles++;
            }
        } else if (!strcmp(line, "SHARE_B64")) snprintf(c->share_b64, 230000, "%s", v);
    }
    fclose(f);
    free(line);
    return n >= cap ? cap : n + 1;
}

static void selftest(const char *golden_path) {
    printf("prl-c-miner %s selftest\n", MINER_VERSION);
    pr_dot_dispatch();

    /* 1. blake3 helpers vs the vendored official implementation */
    {
        uint8_t key[32], msg[64];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
        for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(i * 13 + 5);
        uint8_t a[32], b[32];
        pr_blake3_keyed_block64(key, msg, a);
        blake3_hasher h;
        blake3_hasher_init_keyed(&h, key);
        blake3_hasher_update(&h, msg, 64);
        blake3_hasher_finalize(&h, b, 32);
        CHECK(memcmp(a, b, 32) == 0, "keyed single-block hash == blake3_hasher");

        uint8_t data[5120]; /* 5 chunks: exercises the odd-carry layer pairing */
        for (int i = 0; i < 5000; i++) data[i] = (uint8_t)(i * 31 + 7);
        memset(data + 5000, 0, 120); /* pearl pads to the chunk boundary */
        uint8_t manual[32], lib[32];
        pr_matrix_root(key, data, sizeof(data), manual);
        pr_blake3(data, sizeof(data), key, lib);
        CHECK(memcmp(manual, lib, 32) == 0, "pearl tree root == keyed blake3 (odd-carry tree)");

        /* nbits test vector from the official spm-pow crate */
        pr_u256 t = {{0}};
        /* target = 0x7fff8 << 184 */
        t.w[2] = 0x7fff8ULL << 56;
        t.w[3] = 0x7fff8ULL >> 8;
        uint32_t nb = pr_compact_from_target(&t);
        CHECK(nb == 0x1a07fff8u, "compact(0x7fff8<<184) == 0x1a07fff8");
        pr_u256 back;
        pr_target_from_compact(nb, &back);
        CHECK(pr_u256_cmp(&back, &t) == 0, "expand(0x1a07fff8) == 0x7fff8<<184");
    }

    if (!golden_path) {
        if (fail_count) { printf("selftest FAILED (%d)\n", fail_count); exit(1); }
        printf("selftest PASSED (no golden file given)\n");
        return;
    }

    golden_case *cases = calloc(16, sizeof(golden_case));
    int ncases = golden_load(golden_path, cases, 16);
    if (ncases <= 0) { printf("  FAIL cannot load %s\n", golden_path); exit(1); }
    printf("  %d golden cases\n", ncases);

    for (int ci = 0; ci < ncases; ci++) {
        golden_case *gc = &cases[ci];
        printf("  case %d: m=%u n=%u k=%u seed=%llu\n", ci, gc->m, gc->n, gc->k,
               (unsigned long long)gc->seed);
        char nm[128];

        pr_attempt_mats am;
        am.m = gc->m; am.n = gc->n; am.k = gc->k;
        am.a = malloc((size_t)gc->m * gc->k);
        am.bt = malloc((size_t)gc->n * gc->k);
        am.ap = malloc((size_t)gc->m * gc->k);
        am.btp = malloc((size_t)gc->n * gc->k);
        am.a16 = malloc(sizeof(int16_t) * (size_t)gc->m * gc->k);
        am.bt16 = malloc(sizeof(int16_t) * (size_t)gc->n * gc->k);

        uint8_t header76[76];
        size_t hl = 0;
        pr_hex_decode(header76, 76, gc->header76, &hl);

        pr_fill_base(&am, gc->seed);
        pr_commitment c;
        pr_commit(header76, gc->k, am.a, am.bt, gc->m, gc->n, &c);

        uint8_t cfg[52], buf[256];
        size_t bl = 0;
        pr_config52(gc->k, cfg);
        pr_hex_decode(buf, 256, gc->config52, &bl);
        snprintf(nm, sizeof nm, "case %d config52", ci);
        CHECK(bl == 52 && memcmp(cfg, buf, 52) == 0, nm);
        pr_hex_decode(buf, 256, gc->job_key, &bl);
        snprintf(nm, sizeof nm, "case %d job_key", ci);
        CHECK(memcmp(c.job_key.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->root_a, &bl);
        snprintf(nm, sizeof nm, "case %d root_a", ci);
        CHECK(memcmp(c.root_a.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->root_b, &bl);
        snprintf(nm, sizeof nm, "case %d root_b", ci);
        CHECK(memcmp(c.root_b.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->bound_a, &bl);
        snprintf(nm, sizeof nm, "case %d bound_a", ci);
        CHECK(memcmp(c.bound_a.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->bound_b, &bl);
        snprintf(nm, sizeof nm, "case %d bound_b", ci);
        CHECK(memcmp(c.bound_b.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->b_seed, &bl);
        snprintf(nm, sizeof nm, "case %d b_noise_seed", ci);
        CHECK(memcmp(c.b_noise_seed.b, buf, 32) == 0, nm);
        pr_hex_decode(buf, 256, gc->a_seed, &bl);
        snprintf(nm, sizeof nm, "case %d a_noise_seed", ci);
        CHECK(memcmp(c.a_noise_seed.b, buf, 32) == 0, nm);

        {
            uint8_t exp[64], got[64];
            size_t el = 0;
            pr_hex_decode(exp, 64, gc->a_row0, &el);
            for (int i = 0; i < 64; i++) got[i] = (uint8_t)am.a[i];
            snprintf(nm, sizeof nm, "case %d A row0 bytes", ci);
            CHECK(el == 64 && memcmp(got, exp, 64) == 0, nm);
            pr_hex_decode(exp, 64, gc->bt_row0, &el);
            for (int i = 0; i < 64; i++) got[i] = (uint8_t)am.bt[i];
            snprintf(nm, sizeof nm, "case %d Bᵀ row0 bytes", ci);
            CHECK(el == 64 && memcmp(got, exp, 64) == 0, nm);
        }

        pr_noise_operands(&am, &c);
        {
            uint8_t exp[64], got[64];
            size_t el = 0;
            pr_hex_decode(exp, 64, gc->ap_row0, &el);
            for (int i = 0; i < 64; i++) got[i] = (uint8_t)am.ap[i];
            snprintf(nm, sizeof nm, "case %d A' row0", ci);
            CHECK(el == 64 && memcmp(got, exp, 64) == 0, nm);
            pr_hex_decode(exp, 64, gc->btp_row0, &el);
            for (int i = 0; i < 64; i++) got[i] = (uint8_t)am.btp[i];
            snprintf(nm, sizeof nm, "case %d B'ᵀ row0", ci);
            CHECK(el == 64 && memcmp(got, exp, 64) == 0, nm);
        }
        {
            int8_t fac[PEARL_RANK];
            static const uint8_t LBL_A[32] = "A_tensor";
            pr_uniform_row(LBL_A, c.a_noise_seed.b, 0, PEARL_RANK, fac);
            uint8_t exp[PEARL_RANK], got[PEARL_RANK];
            size_t el = 0;
            pr_hex_decode(exp, PEARL_RANK, gc->al_row0, &el);
            for (int i = 0; i < (int)PEARL_RANK; i++) got[i] = (uint8_t)fac[i];
            snprintf(nm, sizeof nm, "case %d A_L row0", ci);
            CHECK(el == PEARL_RANK && memcmp(got, exp, PEARL_RANK) == 0, nm);
            uint32_t p0[2];
            static const uint8_t LBL_A2[32] = "A_tensor";
            pr_perm_pair(LBL_A2, c.a_noise_seed.b, 0, PEARL_RANK, p0);
            uint32_t px, py;
            sscanf(gc->pairs_a, "%u %u", &px, &py);
            snprintf(nm, sizeof nm, "case %d A_R pair0", ci);
            CHECK(p0[0] == px && p0[1] == py, nm);
        }

        for (int t = 0; t < gc->ntiles; t++) {
            uint32_t tr = gc->tiles[t].tr, tc = gc->tiles[t].tc;
            uint32_t words[16];
            uint8_t digest[32];
            pr_tile_scalar(am.ap, am.btp, gc->k, tr, tc, c.a_noise_seed.b, words, digest);
            snprintf(nm, sizeof nm, "case %d tile(%u,%u) transcript", ci, tr, tc);
            CHECK(memcmp(words, gc->tiles[t].words, 64) == 0, nm);
            char hex[80];
            pr_hex_encode(hex, digest, 32);
            snprintf(nm, sizeof nm, "case %d tile(%u,%u) digest", ci, tr, tc);
            CHECK(strcmp(hex, gc->tiles[t].digest) == 0, nm);
        }

        pr_band_selftest(&am, &c, 0);
        snprintf(nm, sizeof nm, "case %d kernel band0 == scalar tiles", ci);
        CHECK(pr_band_selftest_ok(), nm);

        pr_share sh;
        pr_build_share(&am, &c, gc->tiles[0].tr, gc->tiles[0].tc, &sh);
        snprintf(nm, sizeof nm, "case %d share bincode+base64 == official", ci);
        CHECK(strcmp(sh.b64, gc->share_b64) == 0, nm);
        if (strcmp(sh.b64, gc->share_b64) != 0) {
            size_t l = strlen(sh.b64), le = strlen(gc->share_b64);
            size_t d = 0;
            while (d < l && d < le && sh.b64[d] == gc->share_b64[d]) d++;
            fprintf(stderr, "    first diff at %zu (len ours %zu, official %zu)\n", d, l, le);
            fprintf(stderr, "    ours:     %.100s\n", sh.b64 + (d > 30 ? d - 30 : 0));
            fprintf(stderr, "    official: %.100s\n", gc->share_b64 + (d > 30 ? d - 30 : 0));
        }
        pr_share_free(&sh);

        free(am.a); free(am.bt); free(am.ap); free(am.btp); free(am.a16); free(am.bt16);
    }

#ifdef WITH_GPU
    /* GPU path vs golden: rebuild a golden case through the CUDA backend. The
     * nonce is reconstructed from the golden A's first 8 int7 bytes (the GPU
     * patches exactly those into chunk 0), so device noise, transcripts,
     * digests and the final share must be byte-identical to the CPU/oracle. */
    if (pr_gpu_device_count() > 0) {
        golden_case *gc = NULL;
        for (int i = 0; i < ncases; i++)
            if (cases[i].m == 256 && cases[i].n == 256 && cases[i].k == 2048) { gc = &cases[i]; break; }
        if (gc) {
            char nm[128];
            printf("  gpu selftest (case m=%u n=%u k=%u seed=%llu)\n", gc->m, gc->n, gc->k,
                   (unsigned long long)gc->seed);
            char err[256];
            pr_gpu_ctx *g = pr_gpu_new(gc->m, gc->n, gc->k, err, sizeof err);
            if (!g) {
                printf("  FAIL gpu init: %s\n", err);
                fail_count++;
            } else {
                uint8_t hdr76[76];
                size_t hl = 0;
                pr_hex_decode(hdr76, 76, gc->header76, &hl);
                pr_gpu_set_base_seed(g, gc->seed); /* base matrices == golden A/Bᵀ */
                pr_gpu_set_debug(g, 1);
                if (pr_gpu_set_job(g, hdr76, err, sizeof err) != 0) {
                    printf("  FAIL gpu set_job: %s\n", err);
                    fail_count++;
                } else {
                    /* nonce from A row0's first 8 bytes */
                    uint8_t arow[64];
                    size_t al = 0;
                    pr_hex_decode(arow, 64, gc->a_row0, &al);
                    uint64_t nonce = 0;
                    for (int j = 0; j < 8; j++)
                        nonce |= (uint64_t)(((uint32_t)arow[j] + 64) & 0x7f) << (8 * j);

                    /* force the bound to golden tile(0,0)'s digest so it must hit */
                    uint8_t dig00[32];
                    size_t dl = 0;
                    pr_hex_decode(dig00, 32, gc->tiles[0].digest, &dl);
                    pr_gpu_set_bound(g, dig00);

                    uint64_t coords[PRL_GPU_MAXHITS];
                    uint8_t digs[PRL_GPU_MAXHITS * 32];
                    int nh = pr_gpu_attempt(g, nonce, coords, digs);

                    const pr_commitment *c = pr_gpu_commitment(g);
                    {
                        uint8_t exp[32];
                        size_t el = 0;
                        pr_hex_decode(exp, 32, gc->a_seed, &el);
                        snprintf(nm, sizeof nm, "gpu a_noise_seed == golden");
                        CHECK(el == 32 && memcmp(c->a_noise_seed.b, exp, 32) == 0, nm);
                    }

                    int found = 0;
                    for (int i = 0; i < nh; i++) {
                        if ((coords[i] & 0xffffffffu) == 0 && (coords[i] >> 32) == 0) {
                            found = 1;
                            char hx[80];
                            pr_hex_encode(hx, digs + i * 32, 32);
                            snprintf(nm, sizeof nm, "gpu tile(0,0) digest == golden");
                            CHECK(strcmp(hx, gc->tiles[0].digest) == 0, nm);
                            break;
                        }
                    }
                    snprintf(nm, sizeof nm, "gpu tile(0,0) is a hit");
                    CHECK(found, nm);

                    pr_attempt_mats view;
                    view.m = gc->m; view.n = gc->n; view.k = gc->k;
                    view.a = (int8_t *)pr_gpu_host_a(g);
                    view.bt = (int8_t *)pr_gpu_host_bt(g);
                    view.ap = view.btp = NULL;
                    view.a16 = view.bt16 = NULL;
                    pr_share sh;
                    if (pr_build_share(&view, c, 0, 0, &sh)) {
                        snprintf(nm, sizeof nm, "gpu share bincode+base64 == golden");
                        CHECK(strcmp(sh.b64, gc->share_b64) == 0, nm);
                        pr_share_free(&sh);
                    } else {
                        printf("  FAIL gpu share build\n");
                        fail_count++;
                    }
                }
                pr_gpu_free(g);
            }
        }
    }
#endif

#ifdef WITH_IGPU
    /* iGPU path vs golden: same contract as the GPU section. The KAT (device
     * keyed-BLAKE3 vs the vendored lib) gates the kernel port itself. */
    {
        char reason[192];
        int ndev = pr_igpu_device_count(reason, sizeof reason);
        if (ndev <= 0) {
            printf("  igpu: no OpenCL GPU (%s) — kernel math was covered by igpu-sim\n", reason);
        } else {
            golden_case *gc = NULL;
            for (int i = 0; i < ncases; i++)
                if (cases[i].m == 256 && cases[i].n == 256 && cases[i].k == 2048) {
                    gc = &cases[i];
                    break;
                }
            if (gc) {
                char nm[128];
                printf("  igpu selftest (case m=%u n=%u k=%u seed=%llu)\n", gc->m, gc->n, gc->k,
                       (unsigned long long)gc->seed);
                char err[256];
                pr_igpu_ctx *g = pr_igpu_new(gc->m, gc->n, gc->k, err, sizeof err);
                if (!g) {
                    printf("  FAIL igpu init: %s\n", err);
                    fail_count++;
                } else {
                    uint8_t hdr76[76];
                    size_t hl = 0;
                    pr_hex_decode(hdr76, 76, gc->header76, &hl);
                    pr_igpu_set_base_seed(g, gc->seed);
                    if (pr_igpu_set_job(g, hdr76, err, sizeof err) != 0) {
                        printf("  FAIL igpu set_job: %s\n", err);
                        fail_count++;
                    } else {
                        /* KAT: the device's keyed single-block BLAKE3 */
                        {
                            uint8_t key[32], msg[64], dev[32], ref[32];
                            for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 11 + 3);
                            for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(i * 29 + 1);
                            pr_igpu_debug_hash64(g, key, msg, dev);
                            pr_blake3_keyed_block64(key, msg, ref);
                            snprintf(nm, sizeof nm, "igpu device blake3 KAT");
                            CHECK(memcmp(dev, ref, 32) == 0, nm);
                        }

                        /* nonce from A row0's first 8 int7 bytes */
                        uint8_t arow[64];
                        size_t al = 0;
                        pr_hex_decode(arow, 64, gc->a_row0, &al);
                        uint64_t nonce = 0;
                        for (int j = 0; j < 8; j++)
                            nonce |= (uint64_t)(((uint32_t)arow[j] + 64) & 0x7f) << (8 * j);

                        /* bound = tile(0,0)'s digest so it must hit */
                        uint8_t dig00[32];
                        size_t dl = 0;
                        pr_hex_decode(dig00, 32, gc->tiles[0].digest, &dl);
                        pr_igpu_set_bound(g, dig00);

                        uint64_t coords[PRL_IGPU_MAXHITS];
                        uint8_t digs[PRL_IGPU_MAXHITS * 32];
                        int nh = pr_igpu_attempt(g, nonce, coords, digs);

                        const pr_commitment *c = pr_igpu_commitment(g);
                        {
                            uint8_t exp[32];
                            size_t el = 0;
                            pr_hex_decode(exp, 32, gc->a_seed, &el);
                            snprintf(nm, sizeof nm, "igpu a_noise_seed == golden");
                            CHECK(el == 32 && memcmp(c->a_noise_seed.b, exp, 32) == 0, nm);
                        }
                        {
                            uint8_t ap64[64], exp[64];
                            size_t el = 0;
                            pr_hex_decode(exp, 64, gc->ap_row0, &el);
                            pr_igpu_debug_read_ap(g, ap64, 64);
                            snprintf(nm, sizeof nm, "igpu A' row0 == golden (device noise)");
                            CHECK(el == 64 && memcmp(ap64, exp, 64) == 0, nm);
                        }
                        int found = 0;
                        for (int i = 0; i < nh; i++) {
                            if ((coords[i] & 0xffffffffu) == 0 && (coords[i] >> 32) == 0) {
                                found = 1;
                                char hx[80];
                                pr_hex_encode(hx, digs + i * 32, 32);
                                snprintf(nm, sizeof nm, "igpu tile(0,0) digest == golden");
                                CHECK(strcmp(hx, gc->tiles[0].digest) == 0, nm);
                                break;
                            }
                        }
                        snprintf(nm, sizeof nm, "igpu tile(0,0) is a hit");
                        CHECK(found, nm);

                        pr_attempt_mats view;
                        view.m = gc->m; view.n = gc->n; view.k = gc->k;
                        view.a = (int8_t *)pr_igpu_host_a(g);
                        view.bt = (int8_t *)pr_igpu_host_bt(g);
                        view.ap = view.btp = NULL;
                        view.a16 = view.bt16 = NULL;
                        pr_share sh;
                        if (pr_build_share(&view, c, 0, 0, &sh)) {
                            snprintf(nm, sizeof nm, "igpu share bincode+base64 == golden");
                            CHECK(strcmp(sh.b64, gc->share_b64) == 0, nm);
                            pr_share_free(&sh);
                        } else {
                            printf("  FAIL igpu share build\n");
                            fail_count++;
                        }
                    }
                    pr_igpu_free(g);
                }
            }
        }
    }
#endif

#ifdef WITH_QNPU
    /* NPU path vs golden: same contract as the GPU section. The QNN graph runs
     * the sliced int8 GEMM on the HTP; noise, fold and digest stay on the CPU,
     * so a byte-identical digest proves the NPU MatMuls are exact. */
    {
        char reason[192];
        int ndev = pr_qnpu_device_count(reason, sizeof reason);
        if (ndev <= 0) {
            printf("  qnpu: no QNN HTP device (%s)\n", reason);
        } else {
            golden_case *gc = NULL;
            for (int i = 0; i < ncases; i++)
                if (cases[i].m == 256 && cases[i].n == 256 && cases[i].k == 2048) { gc = &cases[i]; break; }
            if (!gc)
                for (int i = 0; i < ncases; i++)
                    if (cases[i].m <= 256 && cases[i].n <= 256) { gc = &cases[i]; break; }
            if (gc) {
                char nm[128];
                printf("  qnpu selftest (case m=%u n=%u k=%u seed=%llu)\n", gc->m, gc->n, gc->k,
                       (unsigned long long)gc->seed);
                char err[256];
                pr_qnpu_ctx *g = pr_qnpu_new(gc->m, gc->n, gc->k, err, sizeof err);
                if (!g) {
                    printf("  FAIL qnpu init: %s\n", err);
                    fail_count++;
                } else {
                    uint8_t hdr76[76];
                    size_t hl = 0;
                    pr_hex_decode(hdr76, 76, gc->header76, &hl);
                    pr_qnpu_set_base_seed(g, gc->seed); /* base matrices == golden A/Bᵀ */
                    if (pr_qnpu_set_job(g, hdr76, err, sizeof err) != 0) {
                        printf("  FAIL qnpu set_job: %s\n", err);
                        fail_count++;
                    } else {
                        /* nonce from A row0's first 8 bytes */
                        uint8_t arow[64];
                        size_t al = 0;
                        pr_hex_decode(arow, 64, gc->a_row0, &al);
                        uint64_t nonce = 0;
                        for (int j = 0; j < 8; j++)
                            nonce |= (uint64_t)(((uint32_t)arow[j] + 64) & 0x7f) << (8 * j);

                        /* force the bound to golden tile(0,0)'s digest so it must hit */
                        uint8_t dig00[32];
                        size_t dl = 0;
                        pr_hex_decode(dig00, 32, gc->tiles[0].digest, &dl);
                        pr_qnpu_set_bound(g, dig00);

                        uint64_t coords[PRL_QNPU_MAXHITS];
                        uint8_t digs[PRL_QNPU_MAXHITS * 32];
                        int nh = pr_qnpu_attempt(g, nonce, coords, digs);

                        const pr_commitment *c = pr_qnpu_commitment(g);
                        snprintf(nm, sizeof nm, "qnpu a_noise_seed == golden");
                        {
                            uint8_t exp[32];
                            size_t el = 0;
                            pr_hex_decode(exp, 32, gc->a_seed, &el);
                            CHECK(el == 32 && memcmp(c->a_noise_seed.b, exp, 32) == 0, nm);
                        }

                        /* A' row0: the backend keeps A' in slice-major layout
                         * [S][m][128]; reassemble row 0 and compare to golden */
                        {
                            size_t alen = (size_t)gc->m * gc->k;
                            uint8_t *apsm = malloc(alen);
                            uint8_t *aprm = malloc(alen);
                            pr_qnpu_debug_read_ap(g, apsm, alen);
                            const uint32_t S = gc->k / PEARL_RANK;
                            for (uint32_t s = 0; s < S; s++)
                                for (uint32_t i = 0; i < gc->m; i++)
                                    memcpy(aprm + (size_t)i * gc->k + s * PEARL_RANK,
                                           apsm + ((size_t)s * gc->m + i) * PEARL_RANK, PEARL_RANK);
                            uint8_t exp[64];
                            size_t el = 0;
                            pr_hex_decode(exp, 64, gc->ap_row0, &el);
                            snprintf(nm, sizeof nm, "qnpu A' row0 == golden (host noise)");
                            CHECK(el == 64 && memcmp(aprm, exp, 64) == 0, nm);
                            free(apsm);
                            free(aprm);
                        }

                        int hit00 = 0;
                        uint8_t devdig[32] = {0};
                        for (int i = 0; i < nh; i++) {
                            uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                            uint32_t tc = (uint32_t)(coords[i] >> 32);
                            if (tr == 0 && tc == 0) {
                                hit00 = 1;
                                memcpy(devdig, digs + (size_t)i * 32, 32);
                            }
                        }
                        snprintf(nm, sizeof nm, "qnpu tile(0,0) is a hit");
                        CHECK(hit00, nm);
                        snprintf(nm, sizeof nm, "qnpu tile(0,0) digest == golden (NPU GEMM exact)");
                        CHECK(memcmp(devdig, dig00, 32) == 0, nm);

                        pr_attempt_mats view;
                        view.m = gc->m; view.n = gc->n; view.k = gc->k;
                        view.a = (int8_t *)pr_qnpu_host_a(g);
                        view.bt = (int8_t *)pr_qnpu_host_bt(g);
                        view.ap = view.btp = NULL;
                        view.a16 = view.bt16 = NULL;
                        pr_share sh;
                        snprintf(nm, sizeof nm, "qnpu share bincode+base64 == golden");
                        if (pr_build_share(&view, c, gc->tiles[0].tr, gc->tiles[0].tc, &sh)) {
                            CHECK(strcmp(sh.b64, gc->share_b64) == 0, nm);
                            pr_share_free(&sh);
                        } else {
                            printf("  FAIL qnpu share build\n");
                            fail_count++;
                        }
                    }
                    pr_qnpu_free(g);
                }
            } else {
                printf("  qnpu: no small golden case to reuse\n");
            }
        }
    }
#endif

    free(cases);
    if (fail_count) { printf("selftest FAILED (%d checks)\n", fail_count); exit(1); }
    printf("selftest PASSED\n");
}

/* ============================ mining ===================================== */

/* legal attempt shapes: m,n multiples of 64 (chunk-aligned trees, n >= tile
 * span); k a multiple of 64 with 2048 <= k <= 4096 (consensus needs k >= 16r;
 * k <= 4096 keeps every proof within the 64-leaf sibling caps) */
static int shape_valid(uint32_t m, uint32_t n, uint32_t k) {
    if (!m || !n || !k) return 0;
    if (m % 64 || n % 64 || k % 64) return 0;
    if (k < 2048 || k > 4096) return 0;
    if (m > (1u << 24) || n > (1u << 24)) return 0;
    return 1;
}

struct mine_shared {
    pr_ctx *ctx;
    stratum_t *pool;
    uint8_t header[76];
    uint8_t target[32];
    char job_id[80];
    atomic_ullong attempts;
    atomic_uint stop;
    atomic_uint fatal;
    atomic_ullong found;
    atomic_uint cert_warned;
    uint64_t seed0;
    int one_shot; /* offline mode: one attempt per thread */
    int verbose;
#ifdef WITH_GPU
    int gpu;
    pr_gpu_ctx *gctx;
#endif
#ifdef WITH_IGPU
    int igpu;
    pr_igpu_ctx *ictx;
#endif
#ifdef WITH_QNPU
    int qnpu;
    pr_qnpu_ctx *nctx;
#endif
    uint32_t sm, sn, sk; /* attempt shape */
};

static int miner_fatal_exit_status(const struct mine_shared *ms) {
    return atomic_load(&ms->fatal) ? 2 : 0;
}

static void next_seed(uint64_t *s) { *s = *s * 6364136223846793005ULL + 1442695040888963407ULL; }

#ifdef WITH_GPU
/* the GPU is a single worker: one thread drives attempts through the CUDA ctx */
static void *gpu_miner_thread(void *arg) {
    struct { struct mine_shared *ms; unsigned id; } *a = arg;
    struct mine_shared *ms = a->ms;
    pr_gpu_ctx *g = ms->gctx;
    uint64_t nonce = ms->seed0 & 0x00FFFFFFFFFFFFFFULL;
    uint8_t curhdr[76];
    uint8_t curtgt[32];
    int have_job = 0;
    uint64_t coords[PRL_GPU_MAXHITS];
    uint8_t digests[PRL_GPU_MAXHITS * 32];
    char curjid[80] = "";

    while (!atomic_load(&ms->stop)) {
        uint8_t lh[76], lt[32];
        char ljid[80];
        if (ms->pool) {
            uint64_t ver = 0;
            struct stratum_job j;
            stratum_get_job(ms->pool, &j, &ver);
            if (ver == 0) { usleep(200000); continue; }
            if (j.cert_version != 3) {
                if (atomic_exchange(&ms->cert_warned, 1u) == 0)
                    LOGI("job %s has cert_version %u, need 3; waiting for a V3 job",
                         j.job_id, j.cert_version);
                sleep(1);
                continue;
            }
            memcpy(lh, j.header, 76);
            memcpy(lt, j.target_be, 32);
            snprintf(ljid, sizeof(ljid), "%s", j.job_id);
        } else {
            memcpy(lh, ms->header, 76);
            memcpy(lt, ms->target, 32);
            snprintf(ljid, sizeof(ljid), "%s", ms->job_id);
        }

        if (!have_job || memcmp(lh, curhdr, 76) != 0 || memcmp(lt, curtgt, 32) != 0) {
            char err[256];
            pr_u256 bound;
            if (!pr_share_bound(lt, ms->sk, &bound)) {
                LOGI("job %s: share target unusable for k=%u", ljid, ms->sk);
                sleep(2);
                continue;
            }
            if (pr_gpu_set_job(g, lh, err, sizeof err) != 0) {
                LOGI("gpu job setup failed: %s", err);
                sleep(2);
                continue;
            }
            pr_gpu_set_bound(g, (const uint8_t *)bound.w);
            memcpy(curhdr, lh, 76);
            memcpy(curtgt, lt, 32);
            snprintf(curjid, sizeof(curjid), "%s", ljid);
            have_job = 1;
            LOGI("gpu job %s ready (shape %ux%ux%u)", curjid, ms->sm, ms->sn, ms->sk);
        }

        int nh = pr_gpu_attempt(g, nonce, coords, digests);
        atomic_fetch_add(&ms->attempts, 1);

        if (nh > 0) {
            pr_attempt_mats view;
            view.m = ms->sm; view.n = ms->sn; view.k = ms->sk;
            view.a = (int8_t *)pr_gpu_host_a(g);
            view.bt = (int8_t *)pr_gpu_host_bt(g);
            view.ap = view.btp = NULL;
            view.a16 = view.bt16 = NULL;
            const pr_commitment *c = pr_gpu_commitment(g);
            for (int i = 0; i < nh; i++) {
                uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                uint32_t tc = (uint32_t)(coords[i] >> 32);
                atomic_fetch_add(&ms->found, 1);
                LOGI("GPU HIT tile (%u,%u) on job %s", tr, tc, curjid);
                pr_share sh;
                if (pr_build_share(&view, c, tr, tc, &sh)) {
                    if (ms->pool) {
                        if (stratum_send_submit(ms->pool, curjid, sh.b64) != 0)
                            LOGI("submit send failed (connection lost?)");
                        pr_share_free(&sh);
                    } else {
                        printf("plain_proof: %s\n", sh.b64);
                        fflush(stdout);
                        pr_share_free(&sh);
                    }
                }
            }
        }
        if (ms->one_shot) break;
        nonce = (nonce + 1) & 0x00FFFFFFFFFFFFFFULL;
    }
    return NULL;
}
#endif

#ifdef WITH_IGPU
/* the iGPU is a single worker: one thread drives attempts through the OpenCL ctx */
static void *igpu_miner_thread(void *arg) {
    struct { struct mine_shared *ms; unsigned id; } *a = arg;
    struct mine_shared *ms = a->ms;
    pr_igpu_ctx *g = ms->ictx;
    uint64_t nonce = ms->seed0 & 0x00FFFFFFFFFFFFFFULL;
    uint8_t curhdr[76];
    uint8_t curtgt[32];
    int have_job = 0;
    uint64_t coords[PRL_IGPU_MAXHITS];
    uint8_t digests[PRL_IGPU_MAXHITS * 32];
    char curjid[80] = "";

    while (!atomic_load(&ms->stop)) {
        uint8_t lh[76], lt[32];
        char ljid[80];
        if (ms->pool) {
            uint64_t ver = 0;
            struct stratum_job j;
            stratum_get_job(ms->pool, &j, &ver);
            if (ver == 0) { usleep(200000); continue; }
            if (j.cert_version != 3) {
                if (atomic_exchange(&ms->cert_warned, 1u) == 0)
                    LOGI("job %s has cert_version %u, need 3; waiting for a V3 job",
                         j.job_id, j.cert_version);
                sleep(1);
                continue;
            }
            memcpy(lh, j.header, 76);
            memcpy(lt, j.target_be, 32);
            snprintf(ljid, sizeof(ljid), "%s", j.job_id);
        } else {
            memcpy(lh, ms->header, 76);
            memcpy(lt, ms->target, 32);
            snprintf(ljid, sizeof(ljid), "%s", ms->job_id);
        }

        if (!have_job || memcmp(lh, curhdr, 76) != 0 || memcmp(lt, curtgt, 32) != 0) {
            char err[256];
            pr_u256 bound;
            if (!pr_share_bound(lt, ms->sk, &bound)) {
                LOGI("job %s: share target unusable for k=%u", ljid, ms->sk);
                sleep(2);
                continue;
            }
            if (pr_igpu_set_job(g, lh, err, sizeof err) != 0) {
                LOGI("igpu job setup failed: %s", err);
                sleep(2);
                continue;
            }
            pr_igpu_set_bound(g, (const uint8_t *)bound.w);
            memcpy(curhdr, lh, 76);
            memcpy(curtgt, lt, 32);
            snprintf(curjid, sizeof(curjid), "%s", ljid);
            have_job = 1;
            LOGI("igpu job %s ready (shape %ux%ux%u)", curjid, ms->sm, ms->sn, ms->sk);
        }

        int nh = pr_igpu_attempt(g, nonce, coords, digests);
        atomic_fetch_add(&ms->attempts, 1);

        if (nh > 0) {
            pr_attempt_mats view;
            view.m = ms->sm; view.n = ms->sn; view.k = ms->sk;
            view.a = (int8_t *)pr_igpu_host_a(g);
            view.bt = (int8_t *)pr_igpu_host_bt(g);
            view.ap = view.btp = NULL;
            view.a16 = view.bt16 = NULL;
            const pr_commitment *c = pr_igpu_commitment(g);
            for (int i = 0; i < nh; i++) {
                uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                uint32_t tc = (uint32_t)(coords[i] >> 32);
                atomic_fetch_add(&ms->found, 1);
                LOGI("iGPU HIT tile (%u,%u) on job %s", tr, tc, curjid);
                pr_share sh;
                if (pr_build_share(&view, c, tr, tc, &sh)) {
                    if (ms->pool) {
                        if (stratum_send_submit(ms->pool, curjid, sh.b64) != 0)
                            LOGI("submit send failed (connection lost?)");
                        pr_share_free(&sh);
                    } else {
                        printf("plain_proof: %s\n", sh.b64);
                        fflush(stdout);
                        pr_share_free(&sh);
                    }
                }
            }
        }
        if (ms->one_shot) break;
        nonce = (nonce + 1) & 0x00FFFFFFFFFFFFFFULL;
    }
    return NULL;
}
#endif

#ifdef WITH_QNPU
static void qnpu_submit_result(struct mine_shared *ms, pr_qnpu_ctx *g,
                               const char *job_id, uint64_t nonce,
                               int nh, const uint64_t *coords) {
    if (nh <= 0) return;
    pr_attempt_mats view;
    view.m = ms->sm; view.n = ms->sn; view.k = ms->sk;
    view.a = (int8_t *)pr_qnpu_host_a(g);
    view.bt = (int8_t *)pr_qnpu_host_bt(g);
    view.ap = view.btp = NULL;
    view.a16 = view.bt16 = NULL;
    const pr_commitment *commit = pr_qnpu_commitment(g);
    for (int i = 0; i < nh; ++i) {
        const uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
        const uint32_t tc = (uint32_t)(coords[i] >> 32);
        atomic_fetch_add(&ms->found, 1);
        LOGI("NPU HIT tile (%u,%u) nonce %llu on job %s", tr, tc,
             (unsigned long long)nonce, job_id);
        pr_share share;
        if (!pr_build_share(&view, commit, tr, tc, &share)) continue;
        if (ms->pool) {
            if (stratum_send_submit(ms->pool, job_id, share.b64) != 0)
                LOGI("submit send failed (connection lost?)");
        } else {
            printf("plain_proof: %s\n", share.b64);
            fflush(stdout);
        }
        pr_share_free(&share);
    }
}

/* the NPU is a single worker: one thread drives attempts through the QNN ctx.
 * The backend pipelines: attempt_pipe(nonce) queues this attempt on the NPU
 * and returns the PREVIOUS attempt's hits, so the NPU executes while this
 * thread builds the next noise / submits shares — no NPU idle between
 * attempts. Hits are attributed via last_nonce. */
static void *qnpu_miner_thread(void *arg) {
    struct { struct mine_shared *ms; unsigned id; } *a = arg;
    struct mine_shared *ms = a->ms;
    pr_qnpu_ctx *g = ms->nctx;
    uint64_t nonce = ms->seed0 & 0x00FFFFFFFFFFFFFFULL;
    uint8_t curhdr[76];
    uint8_t curtgt[32];
    int have_job = 0;
    uint64_t coords[PRL_QNPU_MAXHITS];
    uint8_t digests[PRL_QNPU_MAXHITS * 32];
    char curjid[80] = "";
    uint64_t last_nonce = 0;
    int has_last = 0;

    while (!atomic_load(&ms->stop)) {
        uint8_t lh[76], lt[32];
        char ljid[80];
        if (ms->pool) {
            uint64_t ver = 0;
            struct stratum_job j;
            stratum_get_job(ms->pool, &j, &ver);
            if (ver == 0) { usleep(200000); continue; }
            if (j.cert_version != 3) {
                if (atomic_exchange(&ms->cert_warned, 1u) == 0)
                    LOGI("job %s has cert_version %u, need 3; waiting for a V3 job",
                         j.job_id, j.cert_version);
                sleep(1);
                continue;
            }
            memcpy(lh, j.header, 76);
            memcpy(lt, j.target_be, 32);
            snprintf(ljid, sizeof(ljid), "%s", j.job_id);
        } else {
            memcpy(lh, ms->header, 76);
            memcpy(lt, ms->target, 32);
            snprintf(ljid, sizeof(ljid), "%s", ms->job_id);
        }

        if (!have_job || memcmp(lh, curhdr, 76) != 0 || memcmp(lt, curtgt, 32) != 0) {
            /* Drain the old job before replacing its B' graph. Otherwise a
             * completed hit can be returned after curjid changes and be
             * submitted under the new job id. */
            if (have_job && has_last) {
                int old_nh = pr_qnpu_drain(g, coords, digests);
                if (old_nh > 0) {
                    pr_attempt_mats old_view;
                    old_view.m = ms->sm; old_view.n = ms->sn; old_view.k = ms->sk;
                    old_view.a = (int8_t *)pr_qnpu_host_a(g);
                    old_view.bt = (int8_t *)pr_qnpu_host_bt(g);
                    old_view.ap = old_view.btp = NULL;
                    old_view.a16 = old_view.bt16 = NULL;
                    const pr_commitment *old_c = pr_qnpu_commitment(g);
                    for (int i = 0; i < old_nh; i++) {
                        uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                        uint32_t tc = (uint32_t)(coords[i] >> 32);
                        atomic_fetch_add(&ms->found, 1);
                        LOGI("NPU HIT tile (%u,%u) nonce %llu on old job %s (job change)",
                             tr, tc, (unsigned long long)last_nonce, curjid);
                        pr_share sh;
                        if (pr_build_share(&old_view, old_c, tr, tc, &sh)) {
                            if (ms->pool) {
                                if (stratum_send_submit(ms->pool, curjid, sh.b64) != 0)
                                    LOGI("submit send failed (connection lost?)");
                                pr_share_free(&sh);
                            } else {
                                printf("plain_proof: %s\n", sh.b64);
                                fflush(stdout);
                                pr_share_free(&sh);
                            }
                        }
                    }
                }
            }
            char err[256];
            pr_u256 bound;
            if (!pr_share_bound(lt, ms->sk, &bound)) {
                LOGI("job %s: share target unusable for k=%u", ljid, ms->sk);
                sleep(2);
                continue;
            }
            if (pr_qnpu_set_job(g, lh, err, sizeof err) != 0) {
                LOGI("npu job setup failed: %s", err);
                sleep(2);
                continue;
            }
            pr_qnpu_set_bound(g, (const uint8_t *)bound.w);
            memcpy(curhdr, lh, 76);
            memcpy(curtgt, lt, 32);
            snprintf(curjid, sizeof(curjid), "%s", ljid);
            have_job = 1;
            LOGI("npu job %s ready (shape %ux%ux%u)", curjid, ms->sm, ms->sn, ms->sk);
            const uint32_t compound_batch = pr_qnpu_compound_attempts(g);
            if (compound_batch)
                LOGI("compound DSP batching active: %u attempts per FastRPC call", compound_batch);
            else if (!ms->one_shot)
                LOGI("compound batching unavailable; using fused single-attempt path");
        }

        const uint32_t compound_count = ms->one_shot ? 0 :
                                        pr_qnpu_compound_attempts(g);
        if (compound_count) {
            const int compound_rc = pr_qnpu_compound_pipe(g, nonce);
            if (compound_rc != 0) {
                LOGI("compound preparation/RPC failed rc=%d; stopping miner without fallback",
                     compound_rc);
                atomic_store(&ms->fatal, 1u);
                atomic_store(&ms->stop, 1u);
                has_last = 0;
                break;
            }

            uint32_t completed = 0;
            int compound_result_failed = 0;
            for (uint32_t attempt = 0; attempt < compound_count; ++attempt) {
                uint64_t item_nonce = 0;
                const int item_hits = pr_qnpu_compound_result(
                    g, attempt, &item_nonce, coords, digests);
                if (item_hits < 0) {
                    LOGI("compound result %u was unavailable; stopping miner",
                         attempt);
                    atomic_store(&ms->fatal, 1u);
                    atomic_store(&ms->stop, 1u);
                    has_last = 0;
                    compound_result_failed = 1;
                    break;
                }
                qnpu_submit_result(ms, g, curjid, item_nonce,
                                   item_hits, coords);
                completed++;
            }
            atomic_fetch_add(&ms->attempts, completed);
            has_last = 0;
            nonce = (nonce + completed) & 0x00FFFFFFFFFFFFFFULL;
            if (compound_result_failed) break;
            if (completed == compound_count) continue;
        }

        uint64_t this_nonce = nonce;
        uint64_t next_nonce = (nonce + 1) & 0x00FFFFFFFFFFFFFFULL;
        int nh = pr_qnpu_attempt_pipe2(g, this_nonce, next_nonce, coords, digests);
        const int npu_failed = pr_qnpu_fused_failed(g);
        if (!npu_failed) atomic_fetch_add(&ms->attempts, 1);

        /* attempt_pipe returns the PREVIOUS queued attempt's hits; the
         * backend's commitment/host_a accessors already reflect it */
        if (nh > 0 && has_last) {
            pr_attempt_mats view;
            view.m = ms->sm; view.n = ms->sn; view.k = ms->sk;
            view.a = (int8_t *)pr_qnpu_host_a(g);
            view.bt = (int8_t *)pr_qnpu_host_bt(g);
            view.ap = view.btp = NULL;
            view.a16 = view.bt16 = NULL;
            const pr_commitment *c = pr_qnpu_commitment(g);
            for (int i = 0; i < nh; i++) {
                uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                uint32_t tc = (uint32_t)(coords[i] >> 32);
                atomic_fetch_add(&ms->found, 1);
                LOGI("NPU HIT tile (%u,%u) nonce %llu on job %s", tr, tc,
                     (unsigned long long)last_nonce, curjid);
                pr_share sh;
                if (pr_build_share(&view, c, tr, tc, &sh)) {
                    if (ms->pool) {
                        if (stratum_send_submit(ms->pool, curjid, sh.b64) != 0)
                            LOGI("submit send failed (connection lost?)");
                        pr_share_free(&sh);
                    } else {
                        printf("plain_proof: %s\n", sh.b64);
                        fflush(stdout);
                        pr_share_free(&sh);
                    }
                }
            }
        }
        if (npu_failed) {
            LOGI("NPU preparation/RPC failed; stopping miner to avoid repeated failed DSP calls");
            atomic_store(&ms->fatal, 1u);
            atomic_store(&ms->stop, 1u);
            break;
        }
        last_nonce = this_nonce;
        has_last = 1;
        if (ms->one_shot) break;
        nonce = (nonce + 1) & 0x00FFFFFFFFFFFFFFULL;
    }

    /* drain: fold the last queued attempt and submit its hits */
    {
        int nh = pr_qnpu_drain(g, coords, digests);
        if (nh > 0 && has_last) {
            pr_attempt_mats view;
            view.m = ms->sm; view.n = ms->sn; view.k = ms->sk;
            view.a = (int8_t *)pr_qnpu_host_a(g);
            view.bt = (int8_t *)pr_qnpu_host_bt(g);
            view.ap = view.btp = NULL;
            view.a16 = view.bt16 = NULL;
            const pr_commitment *c = pr_qnpu_commitment(g);
            for (int i = 0; i < nh; i++) {
                uint32_t tr = (uint32_t)(coords[i] & 0xffffffffu);
                uint32_t tc = (uint32_t)(coords[i] >> 32);
                atomic_fetch_add(&ms->found, 1);
                LOGI("NPU HIT tile (%u,%u) nonce %llu on job %s (drain)", tr, tc,
                     (unsigned long long)last_nonce, curjid);
                pr_share sh;
                if (pr_build_share(&view, c, tr, tc, &sh)) {
                    if (ms->pool) {
                        if (stratum_send_submit(ms->pool, curjid, sh.b64) != 0)
                            LOGI("submit send failed (connection lost?)");
                        pr_share_free(&sh);
                    } else {
                        printf("plain_proof: %s\n", sh.b64);
                        fflush(stdout);
                        pr_share_free(&sh);
                    }
                }
            }
        }
    }
    return NULL;
}
#endif

static void *miner_thread(void *arg) {
#if defined(WITH_GPU) || defined(WITH_IGPU) || defined(WITH_QNPU)
    struct { struct mine_shared *ms; unsigned id; } *a0 = arg;
#ifdef WITH_GPU
    if (a0->ms->gpu) return gpu_miner_thread(arg);
#endif
#ifdef WITH_IGPU
    if (a0->ms->igpu) return igpu_miner_thread(arg);
#endif
#ifdef WITH_QNPU
    if (a0->ms->qnpu) return qnpu_miner_thread(arg);
#endif
#endif
    struct { struct mine_shared *ms; unsigned id; } *a = arg;
    struct mine_shared *ms = a->ms;
    uint64_t seed = ms->seed0 + a->id * 0x9e3779b97f4a7c15ULL;

    while (!atomic_load(&ms->stop)) {
        uint8_t header[76], target[32];
        char job_id[80];
        if (ms->pool) {
            uint64_t ver = 0;
            struct stratum_job j;
            stratum_get_job(ms->pool, &j, &ver);
            if (ver == 0) { usleep(200000); continue; }
            if (j.cert_version != 3) {
                if (atomic_exchange(&ms->cert_warned, 1u) == 0)
                    LOGI("job %s has cert_version %u, need 3; waiting for a V3 job "
                         "(a pool upgrade may be required)", j.job_id, j.cert_version);
                sleep(1);
                continue;
            }
            memcpy(header, j.header, 76);
            memcpy(target, j.target_be, 32);
            snprintf(job_id, sizeof(job_id), "%s", j.job_id);
        } else {
            memcpy(header, ms->header, 76);
            memcpy(target, ms->target, 32);
            snprintf(job_id, sizeof(job_id), "%s", ms->job_id);
        }

        pr_share sh;
        uint32_t hr, hc;
        uint64_t macs = 0;
        bool hit = pr_attempt(ms->ctx, a->id, header, target, job_id, seed, &sh, &hr, &hc, &macs);
        atomic_fetch_add(&ms->attempts, 1);
        next_seed(&seed);

        if (hit) {
            atomic_fetch_add(&ms->found, 1);
            LOGI("thread %u: HIT tile (%u,%u) on job %s (proof %zu B)", a->id, hr, hc, job_id,
                 sh.b64_len);
            if (ms->pool) {
                if (stratum_send_submit(ms->pool, job_id, sh.b64) != 0)
                    LOGI("submit send failed (connection lost?)");
                pr_share_free(&sh);
            } else {
                printf("plain_proof: %s\n", sh.b64);
                fflush(stdout);
                pr_share_free(&sh);
            }
        }
        if (ms->one_shot) break;
    }
    return NULL;
}

static void *pool_reader_thread(void *arg) {
    stratum_t *s = ((struct mine_shared *)arg)->pool;
    for (;;) {
        struct stratum_job j;
        int rc = stratum_poll(s, &j);
        if (rc < 0) {
            LOGI("pool connection lost, reconnecting...");
            sleep(2);
            if (stratum_connect(s) != 0) sleep(5);
            continue;
        }
        if (rc == 0)
            LOGI("new job %s height=%llu diff=%llu cert=v%u", j.job_id,
                 (unsigned long long)j.height, (unsigned long long)j.diff, j.cert_version);
    }
    return NULL;
}

static double sec_since(struct timespec t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0.tv_sec) + 1e-9 * (double)(t.tv_nsec - t0.tv_nsec);
}

static int run_pool(const char *pool_url, const char *wallet, const char *worker, const char *agent,
                    int dialect, int tls, unsigned nthreads, uint32_t m, uint32_t n, uint32_t k) {
    if (!wallet || !*wallet) {
        fprintf(stderr, "error: --wallet is required for pool mining\n");
        return 1;
    }
    pr_dot_dispatch();
    stratum_t *pool = stratum_new(pool_url, wallet, worker, agent, dialect, tls);
    if (!pool) { fprintf(stderr, "error: bad pool url\n"); return 1; }
#ifndef USE_OPENSSL
    if (stratum_is_tls(pool)) {
        fprintf(stderr,
                "error: this build has no TLS support; use a plain-TCP pool such as\n"
                "  br.pearl.herominers.com:1200  (or rebuild with USE_OPENSSL=1)\n");
        return 1;
    }
#endif

    pr_ctx *ctx = pr_ctx_new(m, n, k, nthreads);
    if (!ctx) { fprintf(stderr, "error: out of memory\n"); return 1; }
#ifdef WITH_GPU
    pr_gpu_ctx *gctx = NULL;
    if (g_use_gpu) {
        char err[256];
        if (pr_gpu_device_count() <= 0) { fprintf(stderr, "error: no CUDA device\n"); return 1; }
        gctx = pr_gpu_new(m, n, k, err, sizeof err);
        if (!gctx) { fprintf(stderr, "error: gpu init: %s\n", err); return 1; }
        nthreads = 1; /* the GPU is the worker */
    }
#endif
#ifdef WITH_IGPU
    pr_igpu_ctx *ictx = NULL;
    if (g_use_igpu) {
        char reason[192], err[256];
        if (pr_igpu_device_count(reason, sizeof reason) <= 0) {
            fprintf(stderr, "error: no OpenCL GPU (%s)\n", reason);
            return 1;
        }
        ictx = pr_igpu_new(m, n, k, err, sizeof err);
        if (!ictx) { fprintf(stderr, "error: igpu init: %s\n", err); return 1; }
        nthreads = 1; /* the iGPU is the worker */
    }
#endif
#ifdef WITH_QNPU
    pr_qnpu_ctx *nctx = NULL;
    if (g_use_qnpu) {
        char reason[192], err[256];
        if (pr_qnpu_device_count(reason, sizeof reason) <= 0) {
            fprintf(stderr, "error: no QNN HTP device (%s)\n", reason);
            return 1;
        }
        nctx = pr_qnpu_new(m, n, k, err, sizeof err);
        if (!nctx) { fprintf(stderr, "error: npu init: %s\n", err); return 1; }
        nthreads = 1; /* the NPU is the worker */
    }
#endif

    LOGI("prl-c-miner %s | shape %ux%ux%u | %s | %u thread(s) | pool %s", MINER_VERSION, m, n, k,
#if defined(WITH_GPU) && defined(WITH_IGPU) && defined(WITH_QNPU)
         g_use_gpu ? "GPU" : (g_use_igpu ? "iGPU" : (g_use_qnpu ? "NPU" : "CPU")),
#elif defined(WITH_GPU) && defined(WITH_QNPU)
         g_use_gpu ? "GPU" : (g_use_qnpu ? "NPU" : "CPU"),
#elif defined(WITH_IGPU) && defined(WITH_QNPU)
         g_use_igpu ? "iGPU" : (g_use_qnpu ? "NPU" : "CPU"),
#elif defined(WITH_GPU) && defined(WITH_IGPU)
         g_use_gpu ? "GPU" : (g_use_igpu ? "iGPU" : "CPU"),
#elif defined(WITH_GPU)
         g_use_gpu ? "GPU" : "CPU",
#elif defined(WITH_IGPU)
         g_use_igpu ? "iGPU" : "CPU",
#elif defined(WITH_QNPU)
         g_use_qnpu ? "NPU" : "CPU",
#else
         "CPU",
#endif
         nthreads, pool_url);

    struct mine_shared ms = {0};
    ms.ctx = ctx;
    ms.pool = pool;
    ms.seed0 = (uint64_t)time(NULL) * 1000003ULL;
#ifdef WITH_GPU
    ms.gpu = g_use_gpu;
    ms.gctx = gctx;
#endif
#ifdef WITH_IGPU
    ms.igpu = g_use_igpu;
    ms.ictx = ictx;
#endif
#ifdef WITH_QNPU
    ms.qnpu = g_use_qnpu;
    ms.nctx = nctx;
#endif
    ms.sm = m; ms.sn = n; ms.sk = k;

    if (stratum_connect(pool) != 0) {
        fprintf(stderr, "error: cannot connect to %s\n", pool_url);
        return 1;
    }
    LOGI("connected, authorize sent");

    pthread_t reader;
    pthread_create(&reader, NULL, pool_reader_thread, &ms);

    pthread_t *th = calloc(nthreads, sizeof(pthread_t));
    struct { struct mine_shared *ms; unsigned id; } *ta = calloc(nthreads, sizeof(*ta));
    for (unsigned t = 0; t < nthreads; t++) {
        ta[t].ms = &ms;
        ta[t].id = t;
        pthread_create(&th[t], NULL, miner_thread, &ta[t]);
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t last_macs = 0;
    const unsigned stats_interval = pool_stats_interval_seconds();
    LOGI("pool stats interval %u s", stats_interval);
    while (!atomic_load(&ms.stop)) {
        sleep(stats_interval);
        uint64_t macs = 0;
#ifdef WITH_GPU
        if (g_use_gpu) macs = pr_gpu_macs(gctx);
        else
#endif
#ifdef WITH_IGPU
        if (g_use_igpu) macs = pr_igpu_macs(ictx);
        else
#endif
#ifdef WITH_QNPU
        if (g_use_qnpu) macs = pr_qnpu_macs(nctx);
        else
#endif
        for (unsigned t = 0; t < nthreads; t++) macs += pr_ctx_macs(ctx, t);
        double dt = sec_since(t0);
        LOGI("%.2f GMAC/s | attempts %llu | accepted %llu | rejected %llu | sample_elapsed_s=%.6f delta_macs=%llu", (double)(macs - last_macs) / dt / 1e9,
             (unsigned long long)atomic_load(&ms.attempts), (unsigned long long)stratum_accepted(pool),
             (unsigned long long)stratum_rejected(pool), dt,
             (unsigned long long)(macs - last_macs));
        last_macs = macs;
        clock_gettime(CLOCK_MONOTONIC, &t0);
    }
    const int exit_status = miner_fatal_exit_status(&ms);
    if (exit_status)
        LOGI("fatal NPU error; exiting with status %d", exit_status);
    return exit_status;
}

static int run_offline(const char *job_arg, uint64_t seed, unsigned nthreads, uint32_t m, uint32_t n,
                       uint32_t k) {
    pr_dot_dispatch();
    const char *comma = strchr(job_arg, ',');
    if (!comma) { fprintf(stderr, "bad --job (need header76hex,target64hex)\n"); return 1; }
    uint8_t header[76], target[32];
    size_t l = 0;
    char hb[256];
    snprintf(hb, sizeof(hb), "%.*s", (int)(comma - job_arg), job_arg);
    if (pr_hex_decode(header, 76, hb, &l) != 0 || l != 76) { fprintf(stderr, "bad header hex\n"); return 1; }
    if (pr_hex_decode(target, 32, comma + 1, &l) != 0 || l != 32) { fprintf(stderr, "bad target hex\n"); return 1; }

    pr_ctx *ctx = pr_ctx_new(m, n, k, nthreads);
#ifdef WITH_GPU
    pr_gpu_ctx *gctx = NULL;
    if (g_use_gpu) {
        char err[256];
        gctx = pr_gpu_new(m, n, k, err, sizeof err);
        if (!gctx) { fprintf(stderr, "error: gpu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
#ifdef WITH_IGPU
    pr_igpu_ctx *ictx = NULL;
    if (g_use_igpu) {
        char err[256];
        ictx = pr_igpu_new(m, n, k, err, sizeof err);
        if (!ictx) { fprintf(stderr, "error: igpu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
#ifdef WITH_QNPU
    pr_qnpu_ctx *nctx = NULL;
    if (g_use_qnpu) {
        char err[256];
        nctx = pr_qnpu_new(m, n, k, err, sizeof err);
        if (!nctx) { fprintf(stderr, "error: npu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
    struct mine_shared ms = {0};
    memcpy(ms.header, header, 76);
    memcpy(ms.target, target, 32);
    snprintf(ms.job_id, sizeof(ms.job_id), "offline");
    ms.ctx = ctx;
    ms.seed0 = seed;
    ms.one_shot = 1; /* offline: exactly one attempt per thread */
#ifdef WITH_GPU
    ms.gpu = g_use_gpu;
    ms.gctx = gctx;
#endif
#ifdef WITH_IGPU
    ms.igpu = g_use_igpu;
    ms.ictx = ictx;
#endif
#ifdef WITH_QNPU
    ms.qnpu = g_use_qnpu;
    ms.nctx = nctx;
#endif
    ms.sm = m; ms.sn = n; ms.sk = k;

    pr_u256 bound;
    if (!pr_share_bound(target, k, &bound)) { fprintf(stderr, "target unusable for this k\n"); return 1; }
    uint8_t le[32];
    memcpy(le, bound.w, 32);
    char hex[80];
    pr_hex_encode(hex, le, 32);
    LOGI("offline: shape %ux%ux%u seed %llu bound %s", m, n, k, (unsigned long long)seed, hex);

    pthread_t *th = calloc(nthreads, sizeof(pthread_t));
    struct { struct mine_shared *ms; unsigned id; } *ta = calloc(nthreads, sizeof(*ta));
    for (unsigned t = 0; t < nthreads; t++) {
        ta[t].ms = &ms;
        ta[t].id = t;
        pthread_create(&th[t], NULL, miner_thread, &ta[t]);
    }
    for (unsigned t = 0; t < nthreads; t++) pthread_join(th[t], NULL);
    LOGI("%llu share(s) found; see the HIT lines for tiles", (unsigned long long)atomic_load(&ms.found));
    return atomic_load(&ms.found) ? 0 : 2;
}

static int run_bench(unsigned nthreads, uint32_t m, uint32_t n, uint32_t k, uint64_t seed,
                     double seconds) {
    pr_dot_dispatch();
    pr_ctx *ctx = pr_ctx_new(m, n, k, nthreads);
#ifdef WITH_GPU
    pr_gpu_ctx *gctx = NULL;
    if (g_use_gpu) {
        char err[256];
        gctx = pr_gpu_new(m, n, k, err, sizeof err);
        if (!gctx) { fprintf(stderr, "error: gpu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
#ifdef WITH_IGPU
    pr_igpu_ctx *ictx = NULL;
    if (g_use_igpu) {
        char err[256];
        ictx = pr_igpu_new(m, n, k, err, sizeof err);
        if (!ictx) { fprintf(stderr, "error: igpu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
#ifdef WITH_QNPU
    pr_qnpu_ctx *nctx = NULL;
    if (g_use_qnpu) {
        char err[256];
        nctx = pr_qnpu_new(m, n, k, err, sizeof err);
        if (!nctx) { fprintf(stderr, "error: npu init: %s\n", err); return 1; }
        nthreads = 1;
    }
#endif
    uint8_t header[76] = {0};
    uint32_t version = 0x20000000, ts = 0x66666666, nbits = 0x10000002;
    memcpy(header, &version, 4);
    memset(header + 4, 3, 32);
    memset(header + 36, 4, 32);
    memcpy(header + 68, &ts, 4);
    memcpy(header + 72, &nbits, 4);
    pr_u256 t;
    pr_target_from_compact(nbits, &t);
    uint8_t target[32];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            target[i * 8 + j] = (uint8_t)(t.w[3 - i] >> (56 - 8 * j)); /* BE byte string */

    LOGI("bench: shape %ux%ux%u, %s, %u thread(s), %.0fs", m, n, k,
#if defined(WITH_GPU) && defined(WITH_IGPU) && defined(WITH_QNPU)
         g_use_gpu ? "GPU" : (g_use_igpu ? "iGPU" : (g_use_qnpu ? "NPU" : "CPU")),
#elif defined(WITH_GPU) && defined(WITH_QNPU)
         g_use_gpu ? "GPU" : (g_use_qnpu ? "NPU" : "CPU"),
#elif defined(WITH_IGPU) && defined(WITH_QNPU)
         g_use_igpu ? "iGPU" : (g_use_qnpu ? "NPU" : "CPU"),
#elif defined(WITH_GPU) && defined(WITH_IGPU)
         g_use_gpu ? "GPU" : (g_use_igpu ? "iGPU" : "CPU"),
#elif defined(WITH_GPU)
         g_use_gpu ? "GPU" : "CPU",
#elif defined(WITH_IGPU)
         g_use_igpu ? "iGPU" : "CPU",
#elif defined(WITH_QNPU)
         g_use_qnpu ? "NPU" : "CPU",
#else
         "CPU",
#endif
         nthreads, seconds);
    struct mine_shared ms = {0};
    ms.ctx = ctx;
    ms.seed0 = seed;
    memcpy(ms.header, header, 76);
    memcpy(ms.target, target, 32);
    snprintf(ms.job_id, sizeof(ms.job_id), "bench");
#ifdef WITH_GPU
    ms.gpu = g_use_gpu;
    ms.gctx = gctx;
#endif
#ifdef WITH_IGPU
    ms.igpu = g_use_igpu;
    ms.ictx = ictx;
#endif
#ifdef WITH_QNPU
    ms.qnpu = g_use_qnpu;
    ms.nctx = nctx;
#endif
    ms.sm = m; ms.sn = n; ms.sk = k;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t *th = calloc(nthreads, sizeof(pthread_t));
    struct { struct mine_shared *ms; unsigned id; } *ta = calloc(nthreads, sizeof(*ta));
    atomic_store(&ms.stop, 0);
    for (unsigned t = 0; t < nthreads; t++) {
        ta[t].ms = &ms;
        ta[t].id = t;
        pthread_create(&th[t], NULL, miner_thread, &ta[t]);
    }
    /* interim progress every 15 s: exposes DVFS/throttle dips mid-run */
    {
        uint64_t last_macs = 0;
        struct timespec tl = t0;
        unsigned elapsed = 0;
        while (elapsed < seconds) {
            unsigned step = (unsigned)seconds - elapsed < 15 ? (unsigned)seconds - elapsed : 15;
            sleep(step);
            elapsed += step;
            uint64_t macs = 0;
#ifdef WITH_GPU
            if (g_use_gpu) macs = pr_gpu_macs(gctx);
            else
#endif
#ifdef WITH_IGPU
            if (g_use_igpu) macs = pr_igpu_macs(ictx);
            else
#endif
#ifdef WITH_QNPU
            if (g_use_qnpu) macs = pr_qnpu_macs(nctx);
            else
#endif
            for (unsigned t = 0; t < nthreads; t++) macs += pr_ctx_macs(ctx, t);
            double dt = sec_since(tl);
            LOGI("bench %us: %.2f GMAC/s (avg %.2f)", elapsed,
                 (double)(macs - last_macs) / dt / 1e9, (double)macs / sec_since(t0) / 1e9);
            last_macs = macs;
            clock_gettime(CLOCK_MONOTONIC, &tl);
        }
    }
    atomic_store(&ms.stop, 1);
    for (unsigned t = 0; t < nthreads; t++) pthread_join(th[t], NULL);

    uint64_t macs = 0;
#ifdef WITH_GPU
    if (g_use_gpu) macs = pr_gpu_macs(gctx);
    else
#endif
#ifdef WITH_IGPU
    if (g_use_igpu) macs = pr_igpu_macs(ictx);
    else
#endif
#ifdef WITH_QNPU
    if (g_use_qnpu) macs = pr_qnpu_macs(nctx);
    else
#endif
    for (unsigned t = 0; t < nthreads; t++) macs += pr_ctx_macs(ctx, t);
    double dt = sec_since(t0);
    LOGI("bench done: %.2f s, %llu attempts, %.2f GMAC/s credited (m*n*k per attempt = %llu)",
         dt, (unsigned long long)atomic_load(&ms.attempts), (double)macs / dt / 1e9,
         (unsigned long long)((uint64_t)m * n * (k - k % 128)));
    pr_ctx_free(ctx);
    return 0;
}

int main(int argc, char **argv) {
    const char *pool = NULL, *wallet = NULL, *worker = "cpu", *agent = MINER_AGENT, *golden = NULL,
               *job = NULL;
    int dialect = STRATUM_OBJECT, tls = 0, verbose = 0;
    unsigned nthreads = (unsigned)sysconf(_SC_NPROCESSORS_ONLN);
#ifdef WITH_GPU
    uint32_t m = 4096, n = 262144, k = 4096; /* big-n sweet spot: host overhead amortized, Bᵀ 1 GiB */
#elif defined(WITH_IGPU)
    uint32_t m = 1024, n = 1024, k = 2048; /* 2-CU iGPU sweet spot (~9 ms/attempt) */
#elif defined(WITH_QNPU)
    uint32_t m = 2048, n = 1024, k = 2048; /* NPU sweet spot: m-heavy, B weights packed per job */
#else
    uint32_t m = 2048, n = 2048, k = 2048;
#endif
    uint64_t seed = (uint64_t)time(NULL);
    double bench_seconds = 15;

    /* argument value fetcher: a macro instead of a GCC nested function so the
     * file also builds with clang (Android NDK); semantics identical */
#define NEXTV()                                                                        \
    (i + 1 < argc                                                                      \
         ? argv[++i]                                                                   \
         : (fprintf(stderr, "missing value for %s\n", a), exit(1), (const char *)NULL))
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(USAGE, stdout); return 0; }
        else if (!strcmp(a, "--selftest")) golden = NEXTV();
        else if (!strcmp(a, "-o") || !strcmp(a, "--pool")) pool = NEXTV();
        else if (!strcmp(a, "-u") || !strcmp(a, "--wallet")) wallet = NEXTV();
        else if (!strcmp(a, "-w") || !strcmp(a, "--worker")) worker = NEXTV();
        else if (!strcmp(a, "--agent")) agent = NEXTV();
        else if (!strcmp(a, "--dialect")) {
            const char *d = NEXTV();
            dialect = !strcmp(d, "kryptex") ? STRATUM_KRYPTEX : STRATUM_OBJECT;
        } else if (!strcmp(a, "--tls")) tls = 1;
#ifdef WITH_GPU
        else if (!strcmp(a, "--gpu")) g_use_gpu = 1;
#endif
#ifdef WITH_IGPU
        else if (!strcmp(a, "--igpu")) g_use_igpu = 1;
#endif
#ifdef WITH_QNPU
        else if (!strcmp(a, "--npu")) g_use_qnpu = 1;
#endif
        else if (!strcmp(a, "-t") || !strcmp(a, "--threads")) nthreads = (unsigned)atoi(NEXTV());
        else if (!strcmp(a, "--shape")) sscanf(NEXTV(), "%u,%u,%u", &m, &n, &k);
        else if (!strcmp(a, "--seed")) seed = strtoull(NEXTV(), 0, 10);
        else if (!strcmp(a, "--job")) job = NEXTV();
        else if (!strcmp(a, "--bench")) bench_seconds = atof(NEXTV());
        else if (!strcmp(a, "-v")) verbose = 1;
        else { fprintf(stderr, "unknown arg %s\n\n%s", a, USAGE); return 1; }
    }
    (void)verbose;

    if (!shape_valid(m, n, k)) {
        fprintf(stderr,
                "invalid --shape %u,%u,%u: need multiples of 64, m,n >= 64 and "
                "2048 <= k <= 4096\n",
                m, n, k);
        return 1;
    }

    if (golden) { selftest(golden); return 0; }
    if (job) return run_offline(job, seed, nthreads, m, n, k);
    if (pool) return run_pool(pool, wallet, worker, agent, dialect, tls, nthreads, m, n, k);
    return run_bench(nthreads, m, n, k, seed, bench_seconds);
}
