#ifndef PRL_FUSED_POWER_VOTE_H
#define PRL_FUSED_POWER_VOTE_H

#include <stdint.h>
#include <string.h>
#include "AEEStdErr.h"
#include "HAP_power.h"

/* Only optional firmware API presence is checked here. Destruction always
 * goes through the official HAP_power_destroy wrapper, never a direct call. */
extern int HAP_power_set(void *, HAP_power_request_t *) __attribute__((weak));

#define PRL_HMX_VOTE_DIAG_WORDS 24u
#define PRL_HMX_VOTE_MAGIC UINT32_C(0x48564f54)
#define PRL_HMX_VOTE_VERSION 1u
enum prl_hmx_vote_diag_index {
    PRL_VOTE_MAGIC = 0, PRL_VOTE_VERSION, PRL_VOTE_POLICY,
    PRL_VOTE_BEGIN_RC, PRL_VOTE_END_RC, PRL_VOTE_SET_PRESENT,
    PRL_VOTE_DESTROY_PRESENT, PRL_VOTE_ALLOC_ATTEMPTED,
    PRL_VOTE_CONTEXT_CREATED, PRL_VOTE_DCVS_ATTEMPTED, PRL_VOTE_DCVS_RC,
    PRL_VOTE_HMX_ATTEMPTED, PRL_VOTE_HMX_RC,
    PRL_VOTE_DESTROY_ATTEMPTED, PRL_VOTE_DESTROY_RC,
    PRL_VOTE_CONTEXT_FREED, PRL_VOTE_CONTEXT_ALIVE, PRL_VOTE_POISONED,
    PRL_VOTE_HMX_CORNER, PRL_VOTE_HMX_PICK_DEFAULT,
    PRL_VOTE_HMX_SET_CLOCK, PRL_VOTE_HMX_SET_POWER,
    PRL_VOTE_END_CALLS, PRL_VOTE_BEGIN_REJECTED
};

/* Keep this state alive after a failed end: a static instance in the caller
 * retains both the heap identifier and the poison state across RPC returns. */
typedef struct prl_hmx_vote {
    void *context;
    unsigned poisoned;
    unsigned policy, set_present, destroy_present, allocation_attempted;
    unsigned context_created, dcvs_attempted, hmx_attempted;
    unsigned destroy_attempted, context_freed, end_calls, begin_rejected;
    unsigned hmx_corner, hmx_pick_default, hmx_set_clock, hmx_set_power;
    int begin_rc, end_rc, dcvs_rc, hmx_rc, destroy_rc;
} prl_hmx_vote;

/* Policies: 0=DCVS-only control, 1=Q6-derived HMX default,
 * 2=NOM, 3=TUR, 4=MAX. No policy changes HMX power on/off. */
static inline int prl_hmx_vote_begin(prl_hmx_vote *v, unsigned policy) {
    if (!v) return AEE_EBADPARM;
    if (v->context || v->poisoned) {
        v->begin_rejected = 1;
        return v->begin_rc = AEE_EBADSTATE; /* Never erase a retained identifier. */
    }
    memset(v, 0, sizeof(*v));
    v->policy = policy;
    v->set_present = HAP_power_set != 0;
    v->destroy_present = HAP_power_destroy_client != 0;
    if (policy > 4u) return v->begin_rc = AEE_EBADPARM;
    /* A client must never be created when its formal removal API is absent. */
    if (!v->set_present || !v->destroy_present)
        return v->begin_rc = AEE_EUNSUPPORTEDAPI;
    v->allocation_attempted = 1;
    v->context = HAP_utils_create_context();
    if (!v->context) return v->begin_rc = AEE_ENOMEMORY;
    v->context_created = 1;

    HAP_power_request_t request;
    memset(&request, 0, sizeof(request));
    request.type = HAP_power_set_DCVS_v3;
    request.dcvs_v3.set_dcvs_enable = TRUE;
    request.dcvs_v3.dcvs_enable = TRUE;
    request.dcvs_v3.dcvs_option = HAP_DCVS_V2_PERFORMANCE_MODE;
    request.dcvs_v3.set_latency = TRUE;
    request.dcvs_v3.latency = 100;
    request.dcvs_v3.set_core_params = TRUE;
    request.dcvs_v3.core_params.target_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.core_params.min_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.core_params.max_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.set_bus_params = TRUE;
    request.dcvs_v3.bus_params.target_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.bus_params.min_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.bus_params.max_corner = HAP_DCVS_VCORNER_MAX;
    request.dcvs_v3.set_sleep_disable = TRUE;
    request.dcvs_v3.sleep_disable = HAP_DCVS_LPM_LEVEL1;
    v->dcvs_attempted = 1;
    v->dcvs_rc = HAP_power_set(v->context, &request);
    if (v->dcvs_rc) return v->begin_rc = v->dcvs_rc;
    if (!policy) return 0;

    memset(&request, 0, sizeof(request));
    request.type = HAP_power_set_HMX_v2;
    request.hmx_v2.set_power = FALSE;
    request.hmx_v2.set_clock = TRUE;
    v->hmx_set_clock = 1;
    if (policy == 1u) {
        request.hmx_v2.pick_default = TRUE;
        v->hmx_pick_default = 1;
    } else {
        const HAP_dcvs_exp_voltage_corner_t corner = policy == 2u ? HAP_DCVS_EXP_VCORNER_NOM :
            policy == 3u ? HAP_DCVS_EXP_VCORNER_TUR : HAP_DCVS_EXP_VCORNER_MAX;
        request.hmx_v2.target_corner = corner;
        request.hmx_v2.min_corner = corner;
        request.hmx_v2.max_corner = corner;
        request.hmx_v2.perf_mode = HAP_CLK_PERF_HIGH;
        v->hmx_corner = (unsigned)corner;
    }
    /* Both frequency fields stay zero. pick_default is mutually exclusive
     * with explicit corners, and set_power=FALSE leaves hardware power alone. */
    v->hmx_attempted = 1;
    v->hmx_rc = HAP_power_set(v->context, &request);
    return v->begin_rc = v->hmx_rc;
}

/* Call on every path after begin, including setter failures: an unsuccessful
 * setter may have registered a client before rejecting the requested vote. */
static inline int prl_hmx_vote_end(prl_hmx_vote *v) {
    if (!v) return AEE_EBADPARM;
    v->end_calls++;
    if (!v->context) return v->end_rc = 0;
    v->destroy_attempted++;
    v->destroy_rc = HAP_power_destroy(v->context);
    if (v->destroy_rc) {
        v->poisoned = 1;
        return v->end_rc = v->destroy_rc;
    }
    HAP_utils_destroy_context(v->context);
    v->context = NULL;
    v->context_freed = 1;
    /* Poison is deliberately sticky after any removal failure; a later
     * cleanup retry may release the heap identifier but cannot validate a
     * measurement made while the previous client state was unresolved. */
    return v->end_rc = 0;
}

static inline void prl_hmx_vote_write_diag(const prl_hmx_vote *v,
                                          uint32_t out[PRL_HMX_VOTE_DIAG_WORDS]) {
    memset(out, 0, PRL_HMX_VOTE_DIAG_WORDS * sizeof(*out));
    out[PRL_VOTE_MAGIC] = PRL_HMX_VOTE_MAGIC;
    out[PRL_VOTE_VERSION] = PRL_HMX_VOTE_VERSION;
    out[PRL_VOTE_POLICY] = v->policy;
    out[PRL_VOTE_BEGIN_RC] = (uint32_t)v->begin_rc;
    out[PRL_VOTE_END_RC] = (uint32_t)v->end_rc;
    out[PRL_VOTE_SET_PRESENT] = v->set_present;
    out[PRL_VOTE_DESTROY_PRESENT] = v->destroy_present;
    out[PRL_VOTE_ALLOC_ATTEMPTED] = v->allocation_attempted;
    out[PRL_VOTE_CONTEXT_CREATED] = v->context_created;
    out[PRL_VOTE_DCVS_ATTEMPTED] = v->dcvs_attempted;
    out[PRL_VOTE_DCVS_RC] = (uint32_t)v->dcvs_rc;
    out[PRL_VOTE_HMX_ATTEMPTED] = v->hmx_attempted;
    out[PRL_VOTE_HMX_RC] = (uint32_t)v->hmx_rc;
    out[PRL_VOTE_DESTROY_ATTEMPTED] = v->destroy_attempted;
    out[PRL_VOTE_DESTROY_RC] = (uint32_t)v->destroy_rc;
    out[PRL_VOTE_CONTEXT_FREED] = v->context_freed;
    out[PRL_VOTE_CONTEXT_ALIVE] = v->context != NULL;
    out[PRL_VOTE_POISONED] = v->poisoned;
    out[PRL_VOTE_HMX_CORNER] = v->hmx_corner;
    out[PRL_VOTE_HMX_PICK_DEFAULT] = v->hmx_pick_default;
    out[PRL_VOTE_HMX_SET_CLOCK] = v->hmx_set_clock;
    out[PRL_VOTE_HMX_SET_POWER] = v->hmx_set_power;
    out[PRL_VOTE_END_CALLS] = v->end_calls;
    out[PRL_VOTE_BEGIN_REJECTED] = v->begin_rejected;
}

#endif
