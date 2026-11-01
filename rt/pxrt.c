#define _POSIX_C_SOURCE 200809L

#include "pxrt.h"

#include <stdlib.h>
#include <pthread.h>

/*
 * pxrt.c - the Paxsy runtime: the channel implementation the C
 * backend links against. The ring holds 64-bit slots so every value
 * category the current subset emits (integers, booleans, chars,
 * pointers) travels through one uniform representation; narrower
 * values are sign-extended by the generated code at the send site and
 * truncated at the use site. One mutex guards both counts; the not-
 * full condition wakes senders, the not-empty condition wakes
 * receivers. Close flips a flag and broadcasts both conditions, so
 * blocked participants observe the state change immediately.
 */

struct px_chan {
    pthread_mutex_t mu;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    long long *slots;
    long long cap;
    long long head;     /* next slot to receive */
    long long count;    /* occupied slots */
    int closed;
};

px_chan *px_chan_new(long long cap) {
    px_chan *ch = calloc(1, sizeof(px_chan));
    if (!ch) return NULL;
    if (cap < 1) cap = 1;
    ch->slots = calloc((size_t)cap, sizeof(long long));
    ch->cap = cap;
    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    return ch;
}

int px_chan_send(px_chan *ch, long long v) {
    pthread_mutex_lock(&ch->mu);
    while (!ch->closed && ch->count == ch->cap)
        pthread_cond_wait(&ch->not_full, &ch->mu);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        return -1;
    }
    ch->slots[(ch->head + ch->count) % ch->cap] = v;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

int px_chan_recv(px_chan *ch, long long *out) {
    pthread_mutex_lock(&ch->mu);
    while (!ch->closed && ch->count == 0)
        pthread_cond_wait(&ch->not_empty, &ch->mu);
    if (ch->count == 0) {           /* closed and drained */
        *out = 0;
        pthread_mutex_unlock(&ch->mu);
        return -1;
    }
    *out = ch->slots[ch->head];
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
    return 0;
}

void px_chan_close(px_chan *ch) {
    pthread_mutex_lock(&ch->mu);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_full);
    pthread_cond_broadcast(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
}

void px_chan_free(px_chan *ch) {
    if (!ch) return;
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_full);
    pthread_cond_destroy(&ch->not_empty);
    free(ch->slots);
    free(ch);
}
