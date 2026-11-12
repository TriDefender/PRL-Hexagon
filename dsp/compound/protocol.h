#ifndef PRL_COMPOUND_BATCH_PROTOCOL_H
#define PRL_COMPOUND_BATCH_PROTOCOL_H
#include <stdint.h>
#define PRL_COMPOUND_MAGIC 0x43425031u
#define PRL_COMPOUND_VERSION 3u
#define PRL_COMPOUND_DIAG_WORDS 64u
#define PRL_COMPOUND_ATTEMPT_M 2048u
#define PRL_COMPOUND_ATTEMPT_BYTES 4194304u
#define PRL_COMPOUND_MAX_CHUNK 32u
#define PRL_COMPOUND_RUNTIME_TAG_MAGIC 0x44454500u
#define PRL_COMPOUND_RUNTIME_TAG_MAGIC_MASK 0xffffff00u
static inline int prl_compound_runtime_tag_mode(uint32_t tag,unsigned *mode) {
    if ((tag&PRL_COMPOUND_RUNTIME_TAG_MAGIC_MASK)!=PRL_COMPOUND_RUNTIME_TAG_MAGIC ||
        (tag&0xffu)>1u) return 0;
    if (mode) *mode=tag&0xffu;
    return 1;
}
static inline int prl_compound_runtime_tag_matches(uint32_t tag,int expected_mode,
                                                    unsigned *actual_mode) {
    unsigned mode=0;
    if (!prl_compound_runtime_tag_mode(tag,&mode) ||
        (expected_mode>=0 && (unsigned)expected_mode!=mode)) return 0;
    if (actual_mode) *actual_mode=mode;
    return 1;
}
static inline int prl_compound_parse_expected_deep4_mode(const char *text,int *mode) {
    if (!mode) return 0;
    if (!text) {*mode=-1;return 1;}
    if (text[0]=='0' && text[1]=='\0') {*mode=0;return 1;}
    if (text[0]=='1' && text[1]=='\0') {*mode=1;return 1;}
    return 0;
}
enum {
    CB_MAGIC=0,CB_VERSION,CB_ATTEMPTS,CB_N,CB_FLAGS,CB_MODE,
    CB_CHUNKS_PLANNED,CB_CHUNKS_DONE,CB_ATTEMPTS_DONE,CB_POISON,
    CB_CORE_RC,CB_INIT_RC,CB_BEGIN_RC,CB_END_RC,CB_POLICY,CB_PREPARE,
    CB_PRE_Q6_RC,CB_PRE_Q6_HZ,CB_PRE_HMX_RC,CB_PRE_HMX_HZ,
    CB_POST_Q6_RC,CB_POST_Q6_HZ,CB_POST_HMX_RC,CB_POST_HMX_HZ,
    CB_VOTE_DIAG=24,
    CB_OUTPUT_WORDS=48,CB_WORDS_PER_ATTEMPT,
    CB_SUM_CORE_PCYCLES=50,CB_SUM_STAGE_PCY=51,
    CB_SUM_KERNEL_TOTAL_TICKS=52,CB_SUM_MM_PCY=53,
    CB_SUM_TILE_LOOP_PCY=54,CB_KERNEL_POISON,
    CB_CORE_TICKS=56,CB_SUM_READ_PCY=57,
    CB_OUTER_TICKS=58,CB_SUM_FOLD_PCY=59,
    CB_OUTER_PCYCLES=60,CB_SUM_QUEUE_ACQUIRE_WAIT_PCY=61,
    CB_MAX_CHUNK=62,CB_RUNTIME_KERNEL_TAG=63
};
#endif
