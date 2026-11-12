/* This is only a host test shim, not an ARM implementation or performance model. */
#ifndef PRL_TEST_ARM_NEON_H
#define PRL_TEST_ARM_NEON_H
#include <stdint.h>
#include <string.h>
typedef struct { uint8_t b[8]; } uint8x8_t;
typedef struct { uint8_t b[16]; } uint8x16_t;
typedef struct { uint8x16_t val[4]; } uint8x16x4_t;
static inline uint8x16_t vld1q_u8(const uint8_t *p) { uint8x16_t r;memcpy(r.b,p,16);return r; }
static inline uint8x8_t vld1_u8(const uint8_t *p) { uint8x8_t r;memcpy(r.b,p,8);return r; }
static inline void vst1q_u8(uint8_t *p,uint8x16_t v) { memcpy(p,v.b,16); }
static inline uint8x16_t vdupq_n_u8(uint8_t x) { uint8x16_t r;memset(r.b,x,16);return r; }
static inline uint8x8_t vget_high_u8(uint8x16_t v) { uint8x8_t r;memcpy(r.b,v.b+8,8);return r; }
static inline uint8x16_t vcombine_u8(uint8x8_t lo,uint8x8_t hi) {
    uint8x16_t r;memcpy(r.b,lo.b,8);memcpy(r.b+8,hi.b,8);return r;
}
#define PRL_TEST_VEC_OP(name,op) \
static inline uint8x16_t name(uint8x16_t a,uint8x16_t b) { \
    uint8x16_t r;for (unsigned i=0;i<16;i++) r.b[i]=(uint8_t)(a.b[i] op b.b[i]);return r; }
PRL_TEST_VEC_OP(vaddq_u8,+)
PRL_TEST_VEC_OP(vsubq_u8,-)
PRL_TEST_VEC_OP(vandq_u8,&)
PRL_TEST_VEC_OP(vorrq_u8,|)
PRL_TEST_VEC_OP(veorq_u8,^)
#undef PRL_TEST_VEC_OP
static inline uint8x16_t vqtbl4q_u8(uint8x16x4_t tab,uint8x16_t idx) {
    uint8x16_t r;for (unsigned i=0;i<16;i++) {
        unsigned n=idx.b[i];r.b[i]=n<64?tab.val[n/16].b[n%16]:0;
    }return r;
}
static inline uint8x16_t vzip1q_u8(uint8x16_t a,uint8x16_t b) {
    uint8x16_t r;for(unsigned i=0;i<8;i++){r.b[2*i]=a.b[i];r.b[2*i+1]=b.b[i];}return r;
}
static inline uint8x16_t vzip2q_u8(uint8x16_t a,uint8x16_t b) {
    uint8x16_t r;for(unsigned i=0;i<8;i++){r.b[2*i]=a.b[8+i];r.b[2*i+1]=b.b[8+i];}return r;
}
#endif
