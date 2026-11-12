/* This is NOT one hash of the complete transcript matrix. */
#ifndef PRL_FUSED_BLAKE3_HVX_H
#define PRL_FUSED_BLAKE3_HVX_H
#include <stdint.h>
#ifndef PRL_BLAKE3_HOST_SIM
#include "hexagon_types.h"
#include "hvx_hexagon_protos.h"
#endif

#define PRL_BLAKE3_KEYED_ROOT_FLAGS 27u

static __attribute__((noinline)) void
prl_blake3_keyed_block64x32(const HVX_Vector *message_words,
                           const uint32_t key_words[8],
                           HVX_Vector digest_words[8]) {
    /* Holding all 16 message vectors plus 16 state vectors would leave no temporary registers and cause spills in the 32-register HVX vector file. */
    const volatile HVX_Vector *m=(const volatile HVX_Vector *)message_words;
    HVX_Vector s0=Q6_V_vsplat_R((int32_t)key_words[0]);
    HVX_Vector s1=Q6_V_vsplat_R((int32_t)key_words[1]);
    HVX_Vector s2=Q6_V_vsplat_R((int32_t)key_words[2]);
    HVX_Vector s3=Q6_V_vsplat_R((int32_t)key_words[3]);
    HVX_Vector s4=Q6_V_vsplat_R((int32_t)key_words[4]);
    HVX_Vector s5=Q6_V_vsplat_R((int32_t)key_words[5]);
    HVX_Vector s6=Q6_V_vsplat_R((int32_t)key_words[6]);
    HVX_Vector s7=Q6_V_vsplat_R((int32_t)key_words[7]);
    HVX_Vector s8=Q6_V_vsplat_R((int32_t)0x6a09e667u);
    HVX_Vector s9=Q6_V_vsplat_R((int32_t)0xbb67ae85u);
    HVX_Vector s10=Q6_V_vsplat_R((int32_t)0x3c6ef372u);
    HVX_Vector s11=Q6_V_vsplat_R((int32_t)0xa54ff53au);
    HVX_Vector s12=Q6_V_vzero(),s13=Q6_V_vzero();
    HVX_Vector s14=Q6_V_vsplat_R(64);
    HVX_Vector s15=Q6_V_vsplat_R(PRL_BLAKE3_KEYED_ROOT_FLAGS);
    const HVX_Vector r16=Q6_V_vsplat_R(16),r12=Q6_V_vsplat_R(12);
    const HVX_Vector r8=Q6_V_vsplat_R(8),r7=Q6_V_vsplat_R(7);
#define PRL_B3_G(a,b,c,d,x,y) do { \
    const HVX_Vector mx=m[(x)],my=m[(y)]; \
    (a)=Q6_Vw_vadd_VwVw(Q6_Vw_vadd_VwVw((a),(b)),mx); \
    (d)=Q6_Vuw_vrotr_VuwVuw(Q6_V_vxor_VV((d),(a)),r16); \
    (c)=Q6_Vw_vadd_VwVw((c),(d)); \
    (b)=Q6_Vuw_vrotr_VuwVuw(Q6_V_vxor_VV((b),(c)),r12); \
    (a)=Q6_Vw_vadd_VwVw(Q6_Vw_vadd_VwVw((a),(b)),my); \
    (d)=Q6_Vuw_vrotr_VuwVuw(Q6_V_vxor_VV((d),(a)),r8); \
    (c)=Q6_Vw_vadd_VwVw((c),(d)); \
    (b)=Q6_Vuw_vrotr_VuwVuw(Q6_V_vxor_VV((b),(c)),r7); \
} while (0)
    /* Round 0. */
    PRL_B3_G(s0,s4,s8,s12,0,1);
    PRL_B3_G(s1,s5,s9,s13,2,3);
    PRL_B3_G(s2,s6,s10,s14,4,5);
    PRL_B3_G(s3,s7,s11,s15,6,7);
    PRL_B3_G(s0,s5,s10,s15,8,9);
    PRL_B3_G(s1,s6,s11,s12,10,11);
    PRL_B3_G(s2,s7,s8,s13,12,13);
    PRL_B3_G(s3,s4,s9,s14,14,15);
    /* Round 1. */
    PRL_B3_G(s0,s4,s8,s12,2,6);
    PRL_B3_G(s1,s5,s9,s13,3,10);
    PRL_B3_G(s2,s6,s10,s14,7,0);
    PRL_B3_G(s3,s7,s11,s15,4,13);
    PRL_B3_G(s0,s5,s10,s15,1,11);
    PRL_B3_G(s1,s6,s11,s12,12,5);
    PRL_B3_G(s2,s7,s8,s13,9,14);
    PRL_B3_G(s3,s4,s9,s14,15,8);
    /* Round 2. */
    PRL_B3_G(s0,s4,s8,s12,3,4);
    PRL_B3_G(s1,s5,s9,s13,10,12);
    PRL_B3_G(s2,s6,s10,s14,13,2);
    PRL_B3_G(s3,s7,s11,s15,7,14);
    PRL_B3_G(s0,s5,s10,s15,6,5);
    PRL_B3_G(s1,s6,s11,s12,9,0);
    PRL_B3_G(s2,s7,s8,s13,11,15);
    PRL_B3_G(s3,s4,s9,s14,8,1);
    /* Round 3. */
    PRL_B3_G(s0,s4,s8,s12,10,7);
    PRL_B3_G(s1,s5,s9,s13,12,9);
    PRL_B3_G(s2,s6,s10,s14,14,3);
    PRL_B3_G(s3,s7,s11,s15,13,15);
    PRL_B3_G(s0,s5,s10,s15,4,0);
    PRL_B3_G(s1,s6,s11,s12,11,2);
    PRL_B3_G(s2,s7,s8,s13,5,8);
    PRL_B3_G(s3,s4,s9,s14,1,6);
    /* Round 4. */
    PRL_B3_G(s0,s4,s8,s12,12,13);
    PRL_B3_G(s1,s5,s9,s13,9,11);
    PRL_B3_G(s2,s6,s10,s14,15,10);
    PRL_B3_G(s3,s7,s11,s15,14,8);
    PRL_B3_G(s0,s5,s10,s15,7,2);
    PRL_B3_G(s1,s6,s11,s12,5,3);
    PRL_B3_G(s2,s7,s8,s13,0,1);
    PRL_B3_G(s3,s4,s9,s14,6,4);
    /* Round 5. */
    PRL_B3_G(s0,s4,s8,s12,9,14);
    PRL_B3_G(s1,s5,s9,s13,11,5);
    PRL_B3_G(s2,s6,s10,s14,8,12);
    PRL_B3_G(s3,s7,s11,s15,15,1);
    PRL_B3_G(s0,s5,s10,s15,13,3);
    PRL_B3_G(s1,s6,s11,s12,0,10);
    PRL_B3_G(s2,s7,s8,s13,2,6);
    PRL_B3_G(s3,s4,s9,s14,4,7);
    /* Round 6. */
    PRL_B3_G(s0,s4,s8,s12,11,15);
    PRL_B3_G(s1,s5,s9,s13,5,0);
    PRL_B3_G(s2,s6,s10,s14,1,9);
    PRL_B3_G(s3,s7,s11,s15,8,6);
    PRL_B3_G(s0,s5,s10,s15,14,10);
    PRL_B3_G(s1,s6,s11,s12,2,12);
    PRL_B3_G(s2,s7,s8,s13,3,4);
    PRL_B3_G(s3,s4,s9,s14,7,13);
#undef PRL_B3_G
    digest_words[0]=Q6_V_vxor_VV(s0,s8);
    digest_words[1]=Q6_V_vxor_VV(s1,s9);
    digest_words[2]=Q6_V_vxor_VV(s2,s10);
    digest_words[3]=Q6_V_vxor_VV(s3,s11);
    digest_words[4]=Q6_V_vxor_VV(s4,s12);
    digest_words[5]=Q6_V_vxor_VV(s5,s13);
    digest_words[6]=Q6_V_vxor_VV(s6,s14);
    digest_words[7]=Q6_V_vxor_VV(s7,s15);
}
#endif
