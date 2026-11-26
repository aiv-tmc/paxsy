#include "common.h"
#include "pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * pool.c - pthread worker pool (see pool.h). Workers share one job
 * counter guarded by a single mutex; the critical section is a plain
 * int read/increment, so contention is one lock acquisition per job
 * and the mutex is never held across user work. Thread handles are
 * heap-allocated for the duration of the run because the worker count
 * is only known at runtime (min(jobs, count)). A pthread_create
 * failure degrades to running the remaining jobs inline on the calling
 * thread, so a constrained environment can never lose work.
 */

typedef struct {
    void (*fn)(int index, void *ctx);
    void *ctx;
    int next;   /* next unclaimed job index  */
    int count;  /* total jobs                */
    pthread_mutex_t mu;
} Pool;

/* Claims jobs until the index space is exhausted. Each claim is one
 * short mutex section; the user callback runs entirely outside it. */
static void *pool_worker(void *arg) {
    Pool *p = (Pool *)arg;
    for (;;) {
        int i;
        pthread_mutex_lock(&p->mu);
        i = p->next < p->count ? p->next++ : -1;
        pthread_mutex_unlock(&p->mu);
        if (i < 0) return NULL;
        p->fn(i, p->ctx);
    }
}

void pool_run(void (*fn)(int index, void *ctx), int count, void *ctx) {
    if (count <= 0 || !fn) return;
    long want = 1;
    const char *env = getenv("PAXSY_JOBS");
    if (env && env[0]) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end && *end == '\0' && v > 0) want = v;
    } else {
        long cpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu > 0) want = cpu;
    }
    if (want < 1) want = 1;
    if (want > count) want = count;
    if (want == 1) {
        for (int i = 0; i < count; i++) fn(i, ctx);
        return;
    }
    Pool p;
    p.fn = fn;
    p.ctx = ctx;
    p.next = 0;
    p.count = count;
    pthread_mutex_init(&p.mu, NULL);
    pthread_t *ts = malloc((size_t)want * sizeof(pthread_t));
    if (!ts) {
        pthread_mutex_destroy(&p.mu);
        for (int i = 0; i < count; i++) fn(i, ctx);
        return;
    }
    long started = 0;
    for (long i = 0; i < want; i++)
        if (pthread_create(&ts[i], NULL, pool_worker, &p) == 0)
            started++;
        else
            break;
    if (started == 0) {
        /* No thread could start at all: finish inline, in order. */
        free(ts);
        pthread_mutex_destroy(&p.mu);
        for (int i = 0; i < count; i++) fn(i, ctx);
        return;
    }
    /* A partially started pool still completes every job: unclaimed
     * indices stay in the counter for the live workers, and the caller
     * drains whatever remains after the joins below. */
    for (long i = 0; i < started; i++) pthread_join(ts[i], NULL);
    free(ts);
    pthread_mutex_destroy(&p.mu);
}
