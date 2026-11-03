/*
 * Arbitrary-ratio resampler, see resamp.h.
 *
 * The output at history position t is sum_j x[n + j] g(j - D - mu), with
 * n = floor(t), mu = t - n, D = TAPS/2 - 1 (a fixed delay) and g the
 * Kaiser-windowed sinc low-pass, tabulated for RESAMP_PHASES values of mu.
 */
#include "resamp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* 8-float vectors (AVX with -march=native, two SSE registers otherwise): the
 * dot products are written with them because compilers do not vectorise a
 * float reduction without -ffast-math. */
typedef float v8f __attribute__((vector_size(32)));

static inline v8f load8(const float* p)
{
    v8f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

int resamp_init(struct resamp* r, double ratio, double cutoff)
{
    memset(r, 0, sizeof(*r));
    r->step = ratio;
    r->h = malloc(sizeof(*r->h) * (RESAMP_PHASES + 1));
    r->cap = 1 << 16;
    r->hist = malloc(sizeof(*r->hist) * r->cap);
    if (!r->h || !r->hist) {
        resamp_free(r);
        return -1;
    }
    /* passband edge in cycles per input sample */
    double fc = 0.5 * cutoff / (ratio > 1 ? ratio : 1);
    /* cut halfway into the transition band */
    fc = 0.5 * (fc + 0.5 / (ratio > 1 ? ratio : 1));
    const double beta = 8.0, half = RESAMP_TAPS / 2.0, d = RESAMP_TAPS / 2.0 - 1;
    for (int p = 0; p <= RESAMP_PHASES; p++) {
        const double mu = (double)p / RESAMP_PHASES;
        double sum = 0, g[RESAMP_TAPS];
        for (int j = 0; j < RESAMP_TAPS; j++) {
            const double tau = j - d - mu;
            const double x = 2 * fc * tau;
            const double sinc = fabs(x) < 1e-12 ? 1 : sin(M_PI * x) / (M_PI * x);
            const double w = fabs(tau) < half ? bessel_i0(beta * sqrt(1 - (tau / half) * (tau / half))) / bessel_i0(beta) : 0;
            g[j] = sinc * w;
            sum += g[j];
        }
        for (int j = 0; j < RESAMP_TAPS; j++)
            r->h[p][2 * j] = r->h[p][2 * j + 1] = (float)(g[j] / sum);
    }
    return 0;
}

void resamp_free(struct resamp* r)
{
    free(r->h);
    free(r->hist);
    r->h = NULL;
    r->hist = NULL;
}

int resamp_run(struct resamp* r, const float complex* in, int n, float complex* out, int max_out)
{
    if (r->nhist + n > r->cap) {
        int cap = r->cap;
        while (r->nhist + n > cap)
            cap *= 2;
        float complex* h = realloc(r->hist, sizeof(*h) * cap);
        if (!h)
            return 0;
        r->hist = h;
        r->cap = cap;
    }
    memcpy(r->hist + r->nhist, in, sizeof(*in) * n);
    r->nhist += n;

    int k = 0;
    const float* x = (const float*)r->hist;
    while (k < max_out) {
        const int i = (int)r->t;
        if (i + RESAMP_TAPS > r->nhist)
            break;
        const int p = (int)((r->t - i) * RESAMP_PHASES + 0.5);
        const float* h = r->h[p];
        const float* xi = x + 2 * i;
        /* interleaved I/Q times interleaved taps: even lanes I, odd lanes Q */
        v8f acc = load8(h) * load8(xi);
        for (int j = 8; j < 2 * RESAMP_TAPS; j += 8)
            acc += load8(h + j) * load8(xi + j);
        const float re = (acc[0] + acc[2]) + (acc[4] + acc[6]);
        const float im = (acc[1] + acc[3]) + (acc[5] + acc[7]);
        out[k++] = re + I * im;
        r->t += r->step;
    }
    /* drop the history no longer needed */
    const int drop = (int)r->t;
    if (drop > 0) {
        memmove(r->hist, r->hist + drop, sizeof(*r->hist) * (r->nhist - drop));
        r->nhist -= drop;
        r->t -= drop;
    }
    return k;
}
