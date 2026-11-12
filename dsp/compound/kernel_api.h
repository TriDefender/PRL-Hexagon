#ifndef PRL_COMPOUND_KERNEL_API_H
#define PRL_COMPOUND_KERNEL_API_H
#include <stdint.h>
#define prl_fused_fused_run prl_compound_kernel_run
#define prl_fused_fused_run_hash prl_compound_kernel_run_hash
#include "kernel_gen/fused.h"
#undef prl_fused_fused_run_hash
#undef prl_fused_fused_run
int prl_compound_kernel_initialize(void) __attribute__((visibility("hidden")));
int prl_compound_kernel_poisoned(void) __attribute__((visibility("hidden")));
uint32_t prl_compound_kernel_runtime_tag(void) __attribute__((visibility("hidden")));
#endif
