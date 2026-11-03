/*
 * Channelizer, see chan.h.
 *
 * Blocks of N capture samples, hop H = N - V: the FFT of a block, the bins
 * of a channel moved to 0 Hz and weighted, an L-point inverse FFT, of which
 * the H L / N samples in the middle are kept (the V L / 2N at each end are
 * wrapped-around filter tails).
 */
#include "dvbrx/chan.h"

#include <complex.h>
#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define N 16384       /* capture FFT size (bins of Fs / N) */
#define V 2048        /* overlap: filter impulse responses shorter than this */
#define H (N - V)     /* new capture samples per block */
#define LQ 16         /* channel FFT sizes: multiples of N / (V / 2) */
#define MAX_CH 16

typedef float complex cf;

struct member {
    int used;
    double offset, rate;
    int L, m0, h;    /* inverse FFT size, first kept output, outputs per block */
    int binoff;      /* channel centre, in capture bins */
    int nb;          /* bins used */
    int* k;          /* bin offsets from the centre */
    float* w;        /* weights (including the FFT scaling) */
    fftwf_complex* z;
    fftwf_plan inv;
    float* out;      /* h interleaved I/Q samples */
    double ph, dph;  /* block phase continuity, cycles */
    double fph, dfph; /* residual offset (less than half a bin), cycles per output */
    drx_chan_cb cb;
    void* ctx;
};

struct drx_chan {
    double fs;
    fftwf_complex *in, *x;
    fftwf_plan fwd;
    int fill;
    pthread_mutex_t lock;
    struct member m[MAX_CH];
};

/* FFTW's planner is not thread-safe */
static pthread_mutex_t plan_lock = PTHREAD_MUTEX_INITIALIZER;

drx_chan* drx_chan_new(double fs)
{
    drx_chan* c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fs = fs;
    c->in = fftwf_alloc_complex(N);
    c->x = fftwf_alloc_complex(N);
    if (!c->in || !c->x) {
        drx_chan_free(c);
        return NULL;
    }
    pthread_mutex_lock(&plan_lock);
    c->fwd = fftwf_plan_dft_1d(N, c->in, c->x, FFTW_FORWARD, FFTW_MEASURE);
    pthread_mutex_unlock(&plan_lock);
    memset(c->in, 0, sizeof(fftwf_complex) * N);
    c->fill = V;
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

static void member_free(struct member* m)
{
    if (m->inv) {
        pthread_mutex_lock(&plan_lock);
        fftwf_destroy_plan(m->inv);
        pthread_mutex_unlock(&plan_lock);
    }
    fftwf_free(m->z);
    free(m->k);
    free(m->w);
    free(m->out);
    memset(m, 0, sizeof(*m));
}

void drx_chan_free(drx_chan* c)
{
    if (!c)
        return;
    for (int i = 0; i < MAX_CH; i++)
        if (c->m[i].used)
            member_free(&c->m[i]);
    if (c->fwd) {
        pthread_mutex_lock(&plan_lock);
        fftwf_destroy_plan(c->fwd);
        pthread_mutex_unlock(&plan_lock);
        pthread_mutex_destroy(&c->lock);
    }
    fftwf_free(c->in);
    fftwf_free(c->x);
    free(c);
}

static int smooth7(int n)
{
    static const int f[] = { 2, 3, 5, 7 };
    for (int i = 0; i < 4; i++)
        while (n % f[i] == 0)
            n /= f[i];
    return n == 1;
}

static void place(drx_chan* c, struct member* m, double offset)
{
    m->offset = offset;
    m->binoff = (int)lround(offset / c->fs * N);
    m->dph = fmod((double)m->binoff * H / N, 1.0);
    m->dfph = (offset - (double)m->binoff * c->fs / N) / m->rate;
}

int drx_chan_add(drx_chan* c, double offset, double occ, double rate_want, double* rate,
                 drx_chan_cb cb, void* ctx)
{
    const double fs = c->fs, df = fs / N;
    if (fabs(offset) + occ / 2 >= fs / 2 || occ <= 0)
        return -1;
    /* inverse FFT size: the smallest suitable one at or above the wanted rate */
    int L = (int)ceil(rate_want / df);
    L = (L + LQ - 1) / LQ * LQ;
    while (L < N && !smooth7(L / LQ))
        L += LQ;
    if (L > N)
        L = N;
    if (L * df <= occ)
        return -1;
    int id = 0;
    while (id < MAX_CH && c->m[id].used)
        id++;
    if (id == MAX_CH)
        return -1;
    struct member m = { 0 };
    m.L = L;
    m.rate = L * df;
    m.m0 = (V / 2) * L / N;
    m.h = (int)((long)H * L / N); /* L / LQ whole: m0 and h are too */
    m.cb = cb;
    m.ctx = ctx;
    /* weights: flat over the occupied band, cosine taper to the output
     * Nyquist frequency (and nothing beyond the capture band) */
    const int half = (int)floor(m.rate / 2 / df);
    m.k = malloc(sizeof(int) * (2 * half + 1));
    m.w = malloc(sizeof(float) * (2 * half + 1));
    m.z = fftwf_alloc_complex(L);
    m.out = malloc(sizeof(float) * 2 * m.h);
    if (!m.k || !m.w || !m.z || !m.out) {
        member_free(&m);
        return -1;
    }
    const double pass = occ / 2, stop = m.rate / 2;
    for (int k = -half; k <= half; k++) {
        const double f = fabs(k * df);
        double w = f <= pass ? 1 : f >= stop ? 0 : 0.5 + 0.5 * cos(M_PI * (f - pass) / (stop - pass));
        if (w <= 0)
            continue;
        m.k[m.nb] = k;
        m.w[m.nb] = (float)(w / N);
        m.nb++;
    }
    pthread_mutex_lock(&plan_lock);
    m.inv = fftwf_plan_dft_1d(L, m.z, m.z, FFTW_BACKWARD, smooth7(L) ? FFTW_MEASURE : FFTW_ESTIMATE);
    pthread_mutex_unlock(&plan_lock);
    if (!m.inv) {
        member_free(&m);
        return -1;
    }
    place(c, &m, offset);
    m.used = 1;
    if (rate)
        *rate = m.rate;
    pthread_mutex_lock(&c->lock);
    c->m[id] = m;
    pthread_mutex_unlock(&c->lock);
    return id;
}

void drx_chan_remove(drx_chan* c, int id)
{
    if (id < 0 || id >= MAX_CH)
        return;
    pthread_mutex_lock(&c->lock);
    struct member m = c->m[id];
    memset(&c->m[id], 0, sizeof(c->m[id]));
    pthread_mutex_unlock(&c->lock);
    if (m.used)
        member_free(&m);
}

void drx_chan_set_offset(drx_chan* c, int id, double offset)
{
    if (id < 0 || id >= MAX_CH)
        return;
    pthread_mutex_lock(&c->lock);
    if (c->m[id].used)
        place(c, &c->m[id], offset);
    pthread_mutex_unlock(&c->lock);
}

static void channel_block(drx_chan* c, struct member* m)
{
    const cf* X = (const cf*)c->x;
    cf* Z = (cf*)m->z;
    memset(Z, 0, sizeof(cf) * m->L);
    const cf rot = cexpf(-2.0f * (float)M_PI * (float)m->ph * I);
    m->ph = fmod(m->ph + m->dph, 1.0);
    for (int j = 0; j < m->nb; j++) {
        const int k = m->k[j];
        Z[k < 0 ? k + m->L : k] = X[(m->binoff + k) & (N - 1)] * (m->w[j] * rot);
    }
    fftwf_execute(m->inv);
    /* residual offset: exact phase at the block start, recursion within */
    cf r = cexpf(-2.0f * (float)M_PI * (float)m->fph * I);
    const cf step = cexpf(-2.0f * (float)M_PI * (float)m->dfph * I);
    for (int i = 0; i < m->h; i++) {
        const cf y = Z[m->m0 + i] * r;
        m->out[2 * i] = crealf(y);
        m->out[2 * i + 1] = cimagf(y);
        r *= step;
    }
    m->fph = fmod(m->fph + m->dfph * m->h, 1.0);
    if (m->fph < 0)
        m->fph += 1;
    m->cb(m->ctx, m->out, m->h);
}

void drx_chan_process(drx_chan* c, const float* iq, int n)
{
    pthread_mutex_lock(&c->lock);
    while (n > 0) {
        const int take = N - c->fill < n ? N - c->fill : n;
        memcpy(c->in + c->fill, iq, sizeof(fftwf_complex) * take);
        c->fill += take;
        iq += 2 * take;
        n -= take;
        if (c->fill < N)
            break;
        fftwf_execute(c->fwd);
        for (int i = 0; i < MAX_CH; i++)
            if (c->m[i].used)
                channel_block(c, &c->m[i]);
        memmove(c->in, c->in + H, sizeof(fftwf_complex) * V);
        c->fill = V;
    }
    pthread_mutex_unlock(&c->lock);
}
