/* Each memory operation observes the packet's OLD pointer. */
#ifndef PRL_HMX_MM4_H
#define PRL_HMX_MM4_H
#include <stdint.h>
static inline __attribute__((always_inline))
void prl_hmx_mm4_pointer(const uint8_t *activation,const uint8_t *weight) {
    __asm__ volatile(
        "{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3)\n"
        "  %0 = add(%0,#2048)\n %1 = add(%1,#1024) }\n"
        "{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3)\n"
        "  %0 = add(%0,#2048)\n %1 = add(%1,#1024) }\n"
        "{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3)\n"
        "  %0 = add(%0,#2048)\n %1 = add(%1,#1024) }\n"
        "{ activation.ub = mxmem(%0,%2):cm\n weight.b = mxmem(%1,%3) }\n"
        : "+&r"(activation),"+&r"(weight):"r"(31),"r"(896):"memory");
}
#endif
