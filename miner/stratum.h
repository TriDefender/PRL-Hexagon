/* stratum.h — Pearl pool client interface */
#ifndef STRATUM_H
#define STRATUM_H

#include <stdatomic.h>
#include <stdint.h>

#include "pearl.h"

#define STRATUM_OBJECT 0   /* HeroMiners / LuckyPool */
#define STRATUM_KRYPTEX 1  /* Kryptex */

struct stratum_job {
    char job_id[80];
    uint8_t header[PEARL_HEADER76_LEN];
    uint8_t target_be[32]; /* big-endian 256-bit share target, as sent */
    uint64_t height;
    uint32_t cert_version;
    uint64_t diff;
};

typedef struct stratum stratum_t;

stratum_t *stratum_new(const char *url, const char *wallet, const char *worker, const char *agent,
                       int dialect, int use_tls);
void stratum_free(stratum_t *s);
int stratum_is_tls(stratum_t *s);
int stratum_connect(stratum_t *s);
void stratum_disconnect(stratum_t *s);
/* 0 = new job in *job_out, 1 = other message handled, <0 = connection error */
int stratum_poll(stratum_t *s, struct stratum_job *job_out);
int stratum_send_submit(stratum_t *s, const char *job_id, const char *proof_b64);
void stratum_get_job(stratum_t *s, struct stratum_job *out, uint64_t *version);
uint64_t stratum_job_version(stratum_t *s);
uint64_t stratum_accepted(stratum_t *s);
uint64_t stratum_rejected(stratum_t *s);
uint64_t stratum_submitted(stratum_t *s);

/* minimal JSON field extraction used by the parser and the selftest */
int js_get_string(const char *line, const char *key, char *out, size_t cap);
int js_get_uint(const char *line, const char *key, uint64_t *out);

#endif
