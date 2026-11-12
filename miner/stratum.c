/*
 * stratum.c — Pearl pool client speaking the community stratum dialects
 * (object = HeroMiners/LuckyPool, kryptex = jsonrpc-2.0 subscribe/authorize).
 */
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include "stratum.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define MAX_LINE (2 * 1024 * 1024)
#define RBUF_CAP (256 * 1024)

/* ---------------- minimal JSON field extraction -------------------------- */
/* Values in these lines never contain a raw '"' (hex/targets/base64 alphabet),
 * so scanning for "key" : is sufficient and safe here. */

static const char *find_key(const char *s, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = s;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == ':') q++;
        return q;
    }
    return NULL;
}

int js_get_string(const char *line, const char *key, char *out, size_t cap) {
    const char *p = find_key(line, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            default: c = *p; break;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
        p++;
    }
    if (*p != '"') return -1;
    out[o] = 0;
    return 0;
}

int js_get_uint(const char *line, const char *key, uint64_t *out) {
    const char *p = find_key(line, key);
    if (!p || *p == '"') return -1;
    char *end;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return -1;
    *out = v;
    return 0;
}

/* ---------------- client object ------------------------------------------ */

struct stratum {
    char host[256], port[16];
    char wallet[128], worker[128], agent[128];
    int dialect;
    int use_tls;

    int fd;
#ifdef USE_OPENSSL
    SSL *ssl;
    SSL_CTX *ssl_ctx;
#endif
    char *rbuf;
    size_t rlen;

    pthread_mutex_t wlock;

    pthread_mutex_t slock;
    struct stratum_job job;
    uint64_t job_version;
    uint64_t submit_id;
    atomic_ullong accepted, rejected, submitted;
};

int stratum_job_parse(const char *line, struct stratum_job *j) {
    if (!strstr(line, "\"mining.notify\"")) return -1;
    char hdr[256], tgt[80];
    if (js_get_string(line, "job_id", j->job_id, sizeof(j->job_id)) != 0) return -1;
    if (js_get_string(line, "header", hdr, sizeof(hdr)) != 0) return -1;
    size_t hl = 0;
    if (pr_hex_decode(j->header, PEARL_HEADER76_LEN, hdr, &hl) != 0 || hl != PEARL_HEADER76_LEN) return -1;
    if (js_get_string(line, "target", tgt, sizeof(tgt)) != 0) return -1;
    size_t tl = 0;
    if (pr_hex_decode(j->target_be, 32, tgt, &tl) != 0 || tl != 32) return -1;
    uint64_t h = 0, cv = 0, diff = 0;
    js_get_uint(line, "height", &h);
    js_get_uint(line, "cert_version", &cv);
    js_get_uint(line, "diff", &diff);
    j->height = h;
    j->cert_version = (uint32_t)cv;
    j->diff = diff;
    return 0;
}

/* ---------------- IO ------------------------------------------------------ */

static int conn_read_line(stratum_t *s, char *line, size_t cap) {
    for (;;) {
        for (size_t i = 0; i < s->rlen; i++) {
            if (s->rbuf[i] == '\n') {
                size_t n = i;
                if (n >= cap) return -2;
                memcpy(line, s->rbuf, n);
                line[n] = 0;
                if (n && line[n - 1] == '\r') line[n - 1] = 0;
                memmove(s->rbuf, s->rbuf + i + 1, s->rlen - i - 1);
                s->rlen -= i + 1;
                return 0;
            }
        }
        if (s->rlen >= RBUF_CAP) return -2;
        ssize_t n;
#ifdef USE_OPENSSL
        if (s->ssl) {
            n = SSL_read(s->ssl, s->rbuf + s->rlen, RBUF_CAP - s->rlen);
            if (n <= 0) return -1;
        } else
#endif
        {
            struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
            int ready = poll(&pfd, 1, 60000);
            if (ready == 0) continue; /* an idle pool connection is still usable */
            if (ready < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (!(pfd.revents & POLLIN)) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
                continue;
            }
            n = recv(s->fd, s->rbuf + s->rlen, RBUF_CAP - s->rlen, 0);
            if (n <= 0) return -1;
        }
        s->rlen += (size_t)n;
    }
}

static int conn_send(stratum_t *s, const char *data, size_t len) {
    pthread_mutex_lock(&s->wlock);
    int rc = 0;
    size_t off = 0;
    while (off < len) {
#ifdef USE_OPENSSL
        if (s->ssl) {
            int n = SSL_write(s->ssl, data + off, (int)(len - off));
            if (n <= 0) { rc = -1; break; }
            off += (size_t)n;
            continue;
        }
#endif
        ssize_t n = send(s->fd, data + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) { rc = -1; break; }
        off += (size_t)n;
    }
    pthread_mutex_unlock(&s->wlock);
    return rc;
}

static int conn_send_line(stratum_t *s, const char *line) {
    size_t ll = strlen(line);
    char *buf = malloc(ll + 2);
    if (!buf) abort();
    memcpy(buf, line, ll);
    buf[ll] = '\n';
    buf[ll + 1] = 0;
    int rc = conn_send(s, buf, ll + 1);
    free(buf);
    return rc;
}

int stratum_send_submit(stratum_t *s, const char *job_id, const char *proof_b64) {
    size_t need = strlen(proof_b64) + 512;
    char *line = malloc(need);
    if (!line) abort();
    pthread_mutex_lock(&s->wlock);
    uint64_t id = ++s->submit_id;
    pthread_mutex_unlock(&s->wlock);
    if (s->dialect == STRATUM_KRYPTEX) {
        snprintf(line, need,
                 "{\"id\":%llu,\"jsonrpc\":\"2.0\",\"method\":\"mining.submit\","
                 "\"params\":{\"worker\":\"%s.%s\",\"job_id\":\"%s\",\"plain_proof\":\"%s\"}}",
                 (unsigned long long)id, s->wallet, s->worker, job_id, proof_b64);
    } else {
        snprintf(line, need,
                 "{\"id\":%llu,\"method\":\"mining.submit\","
                 "\"params\":{\"wallet\":\"%s\",\"worker\":\"%s\",\"job_id\":\"%s\",\"plain_proof\":\"%s\"}}",
                 (unsigned long long)id, s->wallet, s->worker, job_id, proof_b64);
    }
    atomic_fetch_add(&s->submitted, 1);
    int rc = conn_send_line(s, line);
    free(line);
    return rc;
}

int stratum_connect(stratum_t *s) {
    /* drop any previous connection first (reconnects would otherwise leak one
     * fd and one SSL object per attempt) */
    pthread_mutex_lock(&s->wlock);
    stratum_disconnect(s);
    pthread_mutex_unlock(&s->wlock);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(s->host, s->port, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "[prl-miner] pool DNS lookup failed for %s:%s: %s\n",
                s->host, s->port, gai ? gai_strerror(gai) : "empty address list");
        return -1;
    }
    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "[prl-miner] pool TCP connect failed for %s:%s: %s\n",
                s->host, s->port, strerror(last_errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int keep_idle = 30, keep_interval = 10, keep_count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
#ifdef TCP_KEEPINTVL
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
#endif
#ifdef TCP_KEEPCNT
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));
#endif
#endif
    s->fd = fd;
    s->rlen = 0;
    s->submit_id = 100; /* replies with id >= 100 are share replies */

#ifdef USE_OPENSSL
    if (s->use_tls) {
        if (!s->ssl_ctx) {
            s->ssl_ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_min_proto_version(s->ssl_ctx, TLS1_2_VERSION);
        }
        s->ssl = SSL_new(s->ssl_ctx);
        SSL_set_fd(s->ssl, fd);
        SSL_set_tlsext_host_name(s->ssl, s->host);
        if (SSL_connect(s->ssl) != 1) {
            SSL_free(s->ssl);
            s->ssl = NULL;
            close(fd);
            s->fd = -1;
            return -2;
        }
    }
#endif

    char line[768];
    if (s->dialect == STRATUM_KRYPTEX) {
        snprintf(line, sizeof(line),
                 "{\"id\":10,\"jsonrpc\":\"2.0\",\"method\":\"mining.subscribe\",\"params\":[\"%s\"]}",
                 s->agent);
        conn_send_line(s, line); /* no reply expected */
        snprintf(line, sizeof(line),
                 "{\"id\":11,\"jsonrpc\":\"2.0\",\"method\":\"mining.authorize\","
                 "\"params\":[\"%s.%s\",\"x\"]}",
                 s->wallet, s->worker);
    } else {
        snprintf(line, sizeof(line),
                 "{\"id\":11,\"method\":\"mining.authorize\",\"params\":{"
                 "\"wallet\":\"%s\",\"worker\":\"%s\",\"agent\":\"%s\"}}",
                 s->wallet, s->worker, s->agent);
    }
    if (conn_send_line(s, line) != 0) {
        fprintf(stderr, "[prl-miner] pool authorize send failed for %s:%s: %s\n",
                s->host, s->port, strerror(errno));
        return -1;
    }
    return 0;
}

void stratum_disconnect(stratum_t *s) {
#ifdef USE_OPENSSL
    if (s->ssl) { SSL_shutdown(s->ssl); SSL_free(s->ssl); s->ssl = NULL; }
#endif
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}

/* returns 0 = new job in *job_out, 1 = other message handled internally, <0 = connection error */
int stratum_poll(stratum_t *s, struct stratum_job *job_out) {
    char *line = malloc(MAX_LINE);
    if (!line) abort();
    int rc = conn_read_line(s, line, MAX_LINE);
    if (rc != 0) { free(line); return -1; }
    if (strstr(line, "\"mining.notify\"")) {
        struct stratum_job j;
        if (stratum_job_parse(line, &j) != 0) { free(line); return 1; }
        pthread_mutex_lock(&s->slock);
        s->job = j;
        s->job_version++;
        pthread_mutex_unlock(&s->slock);
        if (job_out) *job_out = j;
        free(line);
        return 0;
    }
    uint64_t rid = 0;
    js_get_uint(line, "id", &rid);
    if (strstr(line, "\"result\":false") && rid < 100)
        fprintf(stderr, "[prl-miner] pool rejected the authorize (check wallet/worker)\n");
    if (rid >= 100) { /* share reply */
        if (strstr(line, "\"result\":true")) {
            atomic_fetch_add(&s->accepted, 1);
            free(line);
            return 1;
        }
        atomic_fetch_add(&s->rejected, 1);
        fprintf(stderr, "[prl-miner] pool rejected share\n");
        free(line);
        return 1;
    }
    free(line);
    return 1;
}

void stratum_get_job(stratum_t *s, struct stratum_job *out, uint64_t *version) {
    pthread_mutex_lock(&s->slock);
    *out = s->job;
    *version = s->job_version;
    pthread_mutex_unlock(&s->slock);
}

uint64_t stratum_job_version(stratum_t *s) {
    pthread_mutex_lock(&s->slock);
    uint64_t v = s->job_version;
    pthread_mutex_unlock(&s->slock);
    return v;
}

stratum_t *stratum_new(const char *url, const char *wallet, const char *worker, const char *agent,
                       int dialect, int use_tls) {
    stratum_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    const char *h = url;
    if (strncmp(h, "tls://", 6) == 0) { s->use_tls = 1; h += 6; }
    if (use_tls) s->use_tls = 1;
    const char *colon = strrchr(h, ':');
    if (!colon) { free(s); return NULL; }
    snprintf(s->host, sizeof(s->host), "%.*s", (int)(colon - h), h);
    snprintf(s->port, sizeof(s->port), "%s", colon + 1);
    snprintf(s->wallet, sizeof(s->wallet), "%s", wallet);
    snprintf(s->worker, sizeof(s->worker), "%s", worker);
    snprintf(s->agent, sizeof(s->agent), "%s", agent);
    s->dialect = dialect;
    s->fd = -1;
    s->rbuf = malloc(RBUF_CAP);
    if (!s->rbuf) abort();
    pthread_mutex_init(&s->wlock, NULL);
    pthread_mutex_init(&s->slock, NULL);
    return s;
}

void stratum_free(stratum_t *s) {
    if (!s) return;
    stratum_disconnect(s);
    free(s->rbuf);
#ifdef USE_OPENSSL
    if (s->ssl_ctx) SSL_CTX_free(s->ssl_ctx);
#endif
    pthread_mutex_destroy(&s->wlock);
    pthread_mutex_destroy(&s->slock);
    free(s);
}

int stratum_is_tls(stratum_t *s) { return s->use_tls; }
uint64_t stratum_accepted(stratum_t *s) { return atomic_load(&s->accepted); }
uint64_t stratum_rejected(stratum_t *s) { return atomic_load(&s->rejected); }
uint64_t stratum_submitted(stratum_t *s) { return atomic_load(&s->submitted); }
