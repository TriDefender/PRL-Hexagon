/* The frozen candidate clone will select this helper only when PRL_MM4_DEEP=1 in its local snapshot hmx_mm4.h; mode 0 keeps the unchanged four-pair regular helper. */
#ifndef PRL_HMX_MM4_DEEP4_PACKET_H
#define PRL_HMX_MM4_DEEP4_PACKET_H

#include <stdint.h>

#define PRL_HMX_MM4_DEEP4_A_RT 6175u
#define PRL_HMX_MM4_DEEP4_W_RT 3968u
#define PRL_HMX_MM4_DEEP4_K32_TILES 4u

static inline __attribute__((always_inline))
void prl_hmx_mm4_deep4_packet(const uint8_t *activation, const uint8_t *weight) {
    __asm__ volatile(
        "{ activation.ub = mxmem(%0,%2):deep:cm\n"
        "  weight.b = mxmem(%1,%3) }"
        :: "r"(activation), "r"(weight),
           "r"(PRL_HMX_MM4_DEEP4_A_RT), "r"(PRL_HMX_MM4_DEEP4_W_RT)
        : "memory");
}

#endif
