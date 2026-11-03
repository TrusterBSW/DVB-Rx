/*
 * Minimal bounded blocking queue of pointers (mutex + condition variables).
 */
#ifndef DVBRX_QUEUE_H
#define DVBRX_QUEUE_H

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct queue {
    void** items;
    int cap, head, count;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
};

static inline int queue_init(struct queue* q, int cap)
{
    q->items = calloc(cap, sizeof(void*));
    q->cap = cap;
    q->head = q->count = q->closed = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return q->items ? 0 : -1;
}

static inline void queue_destroy(struct queue* q)
{
    free(q->items);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* Wakes up every waiter; pops still return the remaining items, then NULL. */
static inline void queue_close(struct queue* q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

static inline void queue_push(struct queue* q, void* item)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == q->cap && !q->closed)
        pthread_cond_wait(&q->not_full, &q->lock);
    if (!q->closed || q->count < q->cap) {
        q->items[(q->head + q->count) % q->cap] = item;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->lock);
}

/* Pops an item, waiting at most timeout_ms (< 0: forever). Returns NULL on
 * timeout or once the queue is closed and empty. */
static inline void* queue_pop(struct queue* q, int timeout_ms)
{
    void* item = NULL;
    pthread_mutex_lock(&q->lock);
    if (timeout_ms < 0) {
        while (!q->count && !q->closed)
            pthread_cond_wait(&q->not_empty, &q->lock);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        ts.tv_sec += timeout_ms / 1000 + ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        while (!q->count && !q->closed)
            if (pthread_cond_timedwait(&q->not_empty, &q->lock, &ts) == ETIMEDOUT)
                break;
    }
    if (q->count) {
        item = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->lock);
    return item;
}

static inline int queue_closed(struct queue* q)
{
    pthread_mutex_lock(&q->lock);
    int c = q->closed && !q->count;
    pthread_mutex_unlock(&q->lock);
    return c;
}

#endif /* DVBRX_QUEUE_H */
