/*
 * Constellation tap: the last received symbols (after equalisation and
 * carrier recovery) for display. Off unless someone asked within the last
 * few seconds (drx_mt_constellation()): the producers then pay one coarse
 * clock read per block.
 */
#ifndef DVBRX_TAP_H
#define DVBRX_TAP_H

#include <stdatomic.h>
#include <time.h>

#define DRX_TAP_N 4096 /* points kept */

struct drx_tap {
    _Atomic double until; /* CLOCK_MONOTONIC seconds: on before this */
    _Atomic unsigned w;   /* points written so far */
    float buf[2 * DRX_TAP_N];
};

static inline double drx_tap_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static inline int drx_tap_on(struct drx_tap* t)
{
    return t && drx_tap_now() < atomic_load_explicit(&t->until, memory_order_relaxed);
}

/* Points iq[2 i], iq[2 i + 1] for i = 0, step, 2 step... (at most a quarter
 * of the ring per call). The reader tolerates a point being rewritten while
 * it copies: this is for display. */
static inline void drx_tap_put(struct drx_tap* t, const float* iq, int n, int step)
{
    unsigned w = atomic_load_explicit(&t->w, memory_order_relaxed);
    for (int i = 0, lim = DRX_TAP_N / 4; i < n && lim > 0; i += step, lim--) {
        const unsigned k = w++ % DRX_TAP_N;
        t->buf[2 * k] = iq[2 * i];
        t->buf[2 * k + 1] = iq[2 * i + 1];
    }
    atomic_store_explicit(&t->w, w, memory_order_release);
}

#endif
