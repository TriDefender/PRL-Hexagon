#ifndef PRL_EXPERIMENT_COMPOUND_BATCH_PREP_POOL_H
#define PRL_EXPERIMENT_COMPOUND_BATCH_PREP_POOL_H
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef PRL_BATCH_ATTEMPTS
#define PRL_BATCH_ATTEMPTS 4
#endif
#if PRL_BATCH_ATTEMPTS != 4 && PRL_BATCH_ATTEMPTS != 8 && PRL_BATCH_ATTEMPTS != 16 && PRL_BATCH_ATTEMPTS != 32
#error "Compound attempts must be 4, 8, 16, or 32"
#endif
enum {PRL_PREP_ATTEMPTS=PRL_BATCH_ATTEMPTS,PRL_PREP_MAX_WORKERS=4};
typedef int (*prl_prep_task_fn)(void *argument,unsigned attempt);
typedef int (*prl_prep_pin_fn)(int cpu);
typedef struct prl_prep_pool prl_prep_pool;
typedef struct {
    prl_prep_pool *pool;
    pthread_t thread;
    unsigned id,created,ready,quiet;
    int cpu,pin_rc,join_rc;
} prl_prep_worker;
struct prl_prep_pool {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    prl_prep_worker worker[PRL_PREP_MAX_WORKERS];
    unsigned workers,ready_helpers,pending,active,stop,usable,poisoned;
    uint64_t epoch;
    int startup_rc,status;
    prl_prep_pin_fn pin;
    prl_prep_task_fn task;
    void *argument;
};

static int prl_prep_work(prl_prep_task_fn task,void *argument,
                          unsigned id,unsigned workers) {
    for (unsigned a=id;a<PRL_PREP_ATTEMPTS;a+=workers) {
        const int rc=task(argument,a);
        if (rc) return rc;
    }
    return 0;
}
static void *prl_prep_worker_main(void *argument) {
    prl_prep_worker *w=argument;prl_prep_pool *p=w->pool;
    const unsigned id=w->id,workers=p->workers;
    const int pin_rc=p->pin(w->cpu);
    pthread_mutex_lock(&p->mutex);
    w->pin_rc=pin_rc;w->ready=1;p->ready_helpers++;
    if (pin_rc&&!p->startup_rc) p->startup_rc=pin_rc;
    pthread_cond_broadcast(&p->changed);
    uint64_t seen=0;
    while (!pin_rc) {
        while (!p->stop&&seen==p->epoch) pthread_cond_wait(&p->changed,&p->mutex);
        /* Even a concurrent stop must finish an already published job. */
        if (seen!=p->epoch) {
            seen=p->epoch;
            prl_prep_task_fn task=p->task;void *job=p->argument;
            pthread_mutex_unlock(&p->mutex);
            const int rc=prl_prep_work(task,job,id,workers);
            pthread_mutex_lock(&p->mutex);
            if (rc&&!p->status) p->status=rc;
            p->pending--;pthread_cond_broadcast(&p->changed);
        } else if (p->stop) break;
    }
    pthread_mutex_unlock(&p->mutex);
    /* Failed joins may wait for this before returning; no task input/output is touched after this store. */
    __atomic_store_n(&w->quiet,1,__ATOMIC_RELEASE);
    return NULL;
}

/* The caller must finish it before releasing input or metadata. */
static int prl_prep_pool_start(prl_prep_pool **out,unsigned workers,
                               const int cpus[PRL_PREP_MAX_WORKERS],prl_prep_pin_fn pin) {
    if (!out||*out||!cpus||!pin||
        (workers!=1&&workers!=2&&workers!=3&&workers!=4)) return EINVAL;
    prl_prep_pool *p=calloc(1,sizeof(*p));
    if (!p) return ENOMEM;
    int rc=pthread_mutex_init(&p->mutex,NULL);
    if (rc) {free(p);return rc;}
    rc=pthread_cond_init(&p->changed,NULL);
    if (rc) {pthread_mutex_destroy(&p->mutex);free(p);return rc;}
    p->workers=workers;p->pin=pin;*out=p;
    for (unsigned id=0;id<workers;id++) {
        p->worker[id].pool=p;p->worker[id].id=id;p->worker[id].cpu=cpus[id];
        if (!id) continue; /* The coordinator executes worker zero itself. */
        rc=pthread_create(&p->worker[id].thread,NULL,prl_prep_worker_main,p->worker+id);
        if (rc) {
            pthread_mutex_lock(&p->mutex);
            if (!p->startup_rc) p->startup_rc=rc;
            pthread_mutex_unlock(&p->mutex);return rc;
        }
        p->worker[id].created=1;
    }
    pthread_mutex_lock(&p->mutex);
    while (p->ready_helpers!=workers-1u) pthread_cond_wait(&p->changed,&p->mutex);
    rc=p->startup_rc;p->usable=rc==0;
    pthread_mutex_unlock(&p->mutex);
    return rc;
}

/* task/argument are immutable until this returns, including on errors. */
static int prl_prep_pool_run(prl_prep_pool *p,prl_prep_task_fn task,void *argument) {
    if (!p||!task) return EINVAL;
    pthread_mutex_lock(&p->mutex);
    if (!p->usable||p->stop||p->poisoned) {pthread_mutex_unlock(&p->mutex);return ECANCELED;}
    if (p->active) {pthread_mutex_unlock(&p->mutex);return EBUSY;}
    p->active=1;p->pending=p->workers-1u;p->status=0;
    p->task=task;p->argument=argument;p->epoch++;
    pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);
    const int own_rc=prl_prep_work(task,argument,0,p->workers);
    pthread_mutex_lock(&p->mutex);
    if (own_rc&&!p->status) p->status=own_rc;
    while (p->pending) pthread_cond_wait(&p->changed,&p->mutex);
    const int result=p->status;
    p->task=NULL;p->argument=NULL;p->active=0;
    pthread_cond_broadcast(&p->changed);pthread_mutex_unlock(&p->mutex);
    return result;
}

/* Join failure retains the entire heap pool and primitives after all workers become externally quiet. */
static int prl_prep_pool_finish(prl_prep_pool **handle) {
    if (!handle) return EINVAL;
    prl_prep_pool *p=*handle;if (!p) return 0;
    pthread_mutex_lock(&p->mutex);p->stop=1;
    pthread_cond_broadcast(&p->changed);
    while (p->active) pthread_cond_wait(&p->changed,&p->mutex);
    pthread_mutex_unlock(&p->mutex);
    int result=0;
    for (unsigned id=1;id<p->workers;id++) {
        prl_prep_worker *w=p->worker+id;
        if (!w->created) continue;
        const int rc=pthread_join(w->thread,NULL);w->join_rc=rc;
        if (rc) {
            if (!result) result=rc;
            p->poisoned=1;
            while (!__atomic_load_n(&w->quiet,__ATOMIC_ACQUIRE)) sched_yield();
        } else w->created=0;
    }
    if (result) return result;
    pthread_cond_destroy(&p->changed);pthread_mutex_destroy(&p->mutex);
    free(p);*handle=NULL;return 0;
}
#endif
