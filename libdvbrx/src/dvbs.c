/*
 * DVB-S (EN 300 421) receiver back half, see dvbs.h.
 *
 * States: ACQ collects symbols for a 4th-power frequency estimate; SEARCH
 * runs the PLL and tries every (code rate, 90-degree rotation, I/Q swap,
 * puncturing phase) on a block, keeping the one whose decoded bits re-encode
 * closest to the received hard decisions (the 180-degree ambiguity is left
 * to the byte sync, as it only inverts the decoded bits); LOCK decodes the
 * stream continuously.
 */
#include "dvbrx/dvbs.h"
#include "tap.h"
#include "conv.h"
#include "outer.h"
#include "queue.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef float complex cf;

#define ACQ_LEN 16384    /* symbols for the frequency estimate */
#define SEARCH_LEN 4096  /* symbols tried for each hypothesis */
#define SEARCH_MAX_BER 0.2       /* hard-decision mismatch of the best hypothesis */
#define SEARCH_MAX_CONTRAST 0.5  /* best / median mismatch for its rate */
#define PLL_BLOCK 8      /* symbols per decision-directed loop update, see pll_run() */

/* ---- receiver state ------------------------------------------------------- */

enum { ST_ACQ, ST_SEARCH, ST_LOCK };

/* ---- back end: Viterbi -> outer layer -> TS ---------------------------------
 * Runs on its own thread when the receiver is threaded: the front end hands
 * it blocks of depunctured soft pairs tagged with a generation, bumped on
 * every new lock, so that a stale block cannot mix with a new stream. */

#define PB_PAIRS 4096
#define NPB 16

struct pblock {
    int gen, n;
    int16_t pairs[PB_PAIRS][2];
};

struct back {
    int gen;
    struct conv_viterbi vit;
    struct outer outer;
};

static void back_reset(struct back* b, int gen)
{
    b->gen = gen;
    conv_viterbi_reset(&b->vit);
    outer_reset(&b->outer);
}

static void bits_out(void* ctx, const uint8_t* bits, int n)
{
    outer_bits(ctx, bits, n);
}

static void decode_pairs(struct back* b, const int16_t (*pairs)[2], int n)
{
    conv_viterbi_decode(&b->vit, pairs, n, bits_out, &b->outer, &b->outer.active);
}

static int back_init(struct back* b)
{
    const int r = conv_viterbi_init(&b->vit) | outer_init(&b->outer);
    b->outer.active = 0;
    return r;
}

static void back_free(struct back* b)
{
    conv_viterbi_free(&b->vit);
    outer_free(&b->outer);
}

/* ---- receiver ---------------------------------------------------------------- */

struct drx_dvbs {
    struct drx_tap* tap; /* constellation, or NULL */
    enum drx_dvbs_rate fixed_rate;
    int state, search_failures;

    /* carrier */
    cf* acq;
    int nacq;
    double omega; /* rad/symbol */
    cf rot;       /* e^(-j phase) */
    float K1, K2;
    float w_phase[PLL_BLOCK], w_freq[PLL_BLOCK]; /* block loop filter, see pll_run() */
    cf w;           /* e^(-j w_omega), the rotation per symbol of pll_run() */
    double w_omega;
    int w_age;      /* blocks since w was computed exactly */
    float n0;
    long pll_count;

    /* search */
    cf* blk;
    int nblk;
    uint64_t* sdec;
    uint8_t* sbits_out;

    /* locked stream */
    int rate, rot90, swap, gen;
    struct conv_depunct dp;
    struct pblock* cur; /* being filled */

    struct back back;
    int threaded;
    pthread_t thread;
    struct queue free_q, full_q;
    uint64_t pushed;            /* blocks handed to the back end */
    _Atomic uint64_t decoded;   /* blocks it finished */

    uint64_t resyncs;
    double shift; /* upstream correction, cycles/symbol */
};

static void* back_thread(void* arg)
{
    drx_dvbs* d = arg;
    struct pblock* p;
    while ((p = queue_pop(&d->full_q, -1))) {
        if (p->gen != d->back.gen)
            back_reset(&d->back, p->gen);
        if (d->back.outer.active)
            decode_pairs(&d->back, (const int16_t (*)[2])p->pairs, p->n);
        queue_push(&d->free_q, p);
        d->decoded++;
    }
    return NULL;
}

static void pll_weights(drx_dvbs* d);

drx_dvbs* drx_dvbs_new(enum drx_dvbs_rate rate, int threaded)
{
        drx_dvbs* d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->fixed_rate = rate;
    d->acq = malloc(sizeof(cf) * ACQ_LEN);
    d->blk = malloc(sizeof(cf) * SEARCH_LEN);
    d->sdec = malloc(sizeof(*d->sdec) * 8 * SEARCH_LEN);
    d->sbits_out = malloc(8 * SEARCH_LEN);
    int ok = d->acq && d->blk && d->sdec && d->sbits_out && !back_init(&d->back);
    d->back.gen = -1;
    ok = ok && !queue_init(&d->free_q, NPB) && !queue_init(&d->full_q, NPB);
    for (int i = 0; ok && i < NPB; i++) {
        struct pblock* p = malloc(sizeof(*p));
        ok = p != NULL;
        if (ok)
            queue_push(&d->free_q, p);
    }
    if (!ok) {
        drx_dvbs_free(d);
        return NULL;
    }
    /* PLL, Bn*T = 2e-3, damping 0.707 */
    const double bnt = 2e-3, z = M_SQRT1_2;
    const double th = bnt / (z + 1 / (4 * z)), dd = 1 + 2 * z * th + th * th;
    d->K1 = (float)(4 * z * th / dd);
    d->K2 = (float)(4 * th * th / dd);
    pll_weights(d);
    d->state = ST_ACQ;
    d->threaded = threaded;
    if (threaded)
        pthread_create(&d->thread, NULL, back_thread, d);
    return d;
}

/* Hands the block being filled to the back end. */
static void flush_block(drx_dvbs* d)
{
    struct pblock* p = d->cur;
    if (!p || !p->n)
        return;
    d->cur = NULL;
    if (d->threaded) {
        d->pushed++;
        queue_push(&d->full_q, p);
        return;
    }
    if (p->gen != d->back.gen)
        back_reset(&d->back, p->gen);
    if (d->back.outer.active)
        decode_pairs(&d->back, (const int16_t (*)[2])p->pairs, p->n);
    queue_push(&d->free_q, p);
}

void drx_dvbs_set_tap(drx_dvbs* d, struct drx_tap* tap) { d->tap = tap; }

void drx_dvbs_free(drx_dvbs* d)
{
    if (!d)
        return;
    if (d->threaded) {
        flush_block(d);
        queue_close(&d->full_q);
        pthread_join(d->thread, NULL);
    }
    if (d->cur)
        free(d->cur);
    void* p;
    while (d->free_q.items && (p = queue_pop(&d->free_q, 0)))
        free(p);
    while (d->full_q.items && (p = queue_pop(&d->full_q, 0)))
        free(p);
    if (d->free_q.items)
        queue_destroy(&d->free_q);
    if (d->full_q.items)
        queue_destroy(&d->full_q);
    back_free(&d->back);
    free(d->acq);
    free(d->blk);
    free(d->sdec);
    free(d->sbits_out);
    free(d);
}

void drx_dvbs_shift(drx_dvbs* d, double delta)
{
    d->omega -= 2 * M_PI * delta;
    d->shift += delta;
}

void drx_dvbs_get_stats(const drx_dvbs* d, drx_dvbs_stats* s)
{
    memset(s, 0, sizeof(*s));
    s->locked = d->state == ST_LOCK && d->back.outer.synced;
    s->rate = d->state == ST_LOCK ? d->rate : 0;
    s->snr_db = 10.0f * log10f(1.0f / (d->n0 + 1e-20f));
    s->freq = (float)(d->omega / (2 * M_PI) + d->shift);
    s->packets = d->back.outer.packets;
    s->rs_corrected = d->back.outer.rs_corrected;
    s->rs_failed = d->back.outer.rs_failed;
    s->resyncs = d->resyncs;
}

/* ---- carrier ----------------------------------------------------------------- */

static inline cf pll(drx_dvbs* d, cf s)
{
    const cf z = s * d->rot;
    const cf dec = ((crealf(z) >= 0 ? 1.0f : -1.0f) + (cimagf(z) >= 0 ? 1.0f : -1.0f) * I) *
                   (float)M_SQRT1_2;
    const float e = cimagf(z * conjf(dec));
    if ((d->pll_count & 15) == 0) {
        const cf err = z - dec;
        d->n0 += 1.6e-3f * (crealf(err * conjf(err)) - d->n0);
    }
    const float phi = (float)(d->omega + d->K1 * e);
    d->omega += d->K2 * e;
    /* exact, as pll_run(): a second-order expansion advances by more than
     * phi at a large offset, and omega would settle off the true frequency */
    d->rot *= cexpf(-I * phi);
    if ((++d->pll_count & 255) == 0)
        d->rot /= cabsf(d->rot);
    return z;
}

/* Block loop filter weights, as in pl.c: with L the unit lower triangular
 * matrix L_it = K1 + K2 (i - 1 - t) (t < i), the errors e measured on a
 * block with the rotation frozen at its start (advancing by omega only)
 * are L e', e' those the symbol-by-symbol loop would have seen; its phase
 * advance K1 sum e' + K2 sum (n - 1 - i) e'_i and frequency step K2 sum e'
 * are w_phase . e and w_freq . e. */
static void pll_weights(drx_dvbs* d)
{
    const int n = PLL_BLOCK;
    for (int j = 0; j < n; j++) {
        double x[PLL_BLOCK]; /* column j of L^-1 */
        for (int i = 0; i < n; i++) {
            double v = i == j;
            for (int t = 0; t < i; t++)
                v -= (d->K1 + d->K2 * (i - 1 - t)) * x[t];
            x[i] = v;
        }
        double a = 0, b = 0;
        for (int i = 0; i < n; i++) {
            a += x[i];
            b += (n - 1 - i) * x[i];
        }
        d->w_phase[j] = (float)(d->K1 * a + d->K2 * b);
        d->w_freq[j] = (float)(d->K2 * a);
    }
}

/* The loop of pll() over n symbols, PLL_BLOCK at a time: the per-symbol
 * loop is a chain of dependent operations (~11 ns a symbol); here the
 * symbols of a block are independent (vectorised) and the loop is
 * updated once per block with the weights of pll_weights(), the same in
 * the linear (locked) regime. The rotation within a block follows the
 * whole frequency estimate exactly (powers of e^(-j omega): at a low
 * symbol rate omega can be tenths of a radian per symbol). */
static void pll_run(drx_dvbs* d, const cf* s, cf* out, int n)
{
    _Static_assert(PLL_BLOCK == 8, "the powers of w below are written for 8");
    const float c = (float)M_SQRT1_2;
    int k = 0;
    for (; k + PLL_BLOCK <= n; k += PLL_BLOCK) {
        /* e^(-j omega): omega moves by ~1e-5 rad a block, so w follows it by
         * a small rotation; computed again exactly every 64 blocks, or when
         * omega jumps (a correction from the front end) */
        const double dw = d->omega - d->w_omega;
        if (fabs(dw) > 1e-3 || ++d->w_age >= 64) {
            d->w = cexpf(-I * (float)d->omega);
            d->w_age = 0;
        } else {
            d->w *= 1.0f - (float)dw * I - 0.5f * (float)(dw * dw);
        }
        d->w_omega = d->omega;
        /* rotations rot w^i, i = 0..7, by doubling (depth 3, not 7) */
        const cf w1 = d->w, w2 = w1 * w1, w4 = w2 * w2;
        const cf p[PLL_BLOCK] = { 1.0f, w1, w2, w2 * w1, w4, w4 * w1, w4 * w2, w4 * w2 * w1 };
        const cf rot = d->rot;
        /* plain float arrays: GCC vectorises these loops (complex types it
         * does not) */
        float rr[PLL_BLOCK], ri[PLL_BLOCK], sr[PLL_BLOCK], si[PLL_BLOCK];
        for (int i = 0; i < PLL_BLOCK; i++) {
            const cf r = rot * p[i];
            rr[i] = crealf(r);
            ri[i] = cimagf(r);
            sr[i] = crealf(s[k + i]);
            si[i] = cimagf(s[k + i]);
        }
        float zr[PLL_BLOCK], zi[PLL_BLOCK], e[PLL_BLOCK], q[PLL_BLOCK];
        for (int i = 0; i < PLL_BLOCK; i++) {
            zr[i] = sr[i] * rr[i] - si[i] * ri[i];
            zi[i] = sr[i] * ri[i] + si[i] * rr[i];
            /* dec = (sign(re) + j sign(im)) / sqrt 2; e = Im(z conj(dec)) */
            const float dr = zr[i] >= 0 ? c : -c, di = zi[i] >= 0 ? c : -c;
            e[i] = zi[i] * dr - zr[i] * di;
            q[i] = (zr[i] - dr) * (zr[i] - dr) + (zi[i] - di) * (zi[i] - di);
        }
        float sum = 0, step = 0, err2 = 0;
        for (int i = 0; i < PLL_BLOCK; i++) {
            sum += d->w_phase[i] * e[i];
            step += d->w_freq[i] * e[i];
            err2 += q[i];
        }
        for (int i = 0; i < PLL_BLOCK; i++)
            out[k + i] = zr[i] + zi[i] * I;
        /* noise estimate: the block's mean error, with the time constant of
         * pll()'s (1.6e-3 every 16 symbols) */
        d->n0 += 1.6e-3f * (PLL_BLOCK / 16.0f) * (err2 / PLL_BLOCK - d->n0);
        d->rot = rot * p[PLL_BLOCK - 1] * w1 * (1.0f - sum * I - 0.5f * sum * sum);
        d->omega += step;
        d->pll_count += PLL_BLOCK;
        if ((d->pll_count & 255) < PLL_BLOCK)
            d->rot /= cabsf(d->rot);
    }
    for (; k < n; k++)
        out[k] = pll(d, s[k]);
}

static void estimate_freq(drx_dvbs* d)
{
    cf r = 0, prev = 0;
    for (int k = 0; k < ACQ_LEN; k++) {
        cf z2 = d->acq[k] * d->acq[k];
        cf z4 = z2 * z2;
        if (k)
            r += z4 * conjf(prev);
        prev = z4;
    }
    d->omega = cargf(r) / 4;
    d->rot = 1.0f;
    d->w_omega = 1e9; /* computed again at the next block */
    d->n0 = 1.0f;
}

/* ---- hypotheses ------------------------------------------------------------ */

/* Symbol after the rotation/swap hypothesis, as two soft bits (I, Q). */
static inline void soft_bits(cf z, int rot90, int swap, int16_t* s)
{
    if (swap)
        z = conjf(z);
    if (rot90)
        z *= I;
    s[0] = conv_soft(crealf(z) * (float)M_SQRT2);
    s[1] = conv_soft(cimagf(z) * (float)M_SQRT2);
}

/* Decode the search block with one hypothesis; returns the fraction of
 * kept coded bits whose hard decision disagrees with the re-encoded output. */
static double try_hypothesis(drx_dvbs* d, int rate, int rot90, int swap, int offset)
{
    int16_t soft[2 * SEARCH_LEN];
    for (int k = 0; k < d->nblk; k++)
        soft_bits(d->blk[k], rot90, swap, soft + 2 * k);
    struct conv_depunct dp = { .p = &CONV_PUNCT[rate], .pos = offset };
    static __thread int16_t pairs[8 * SEARCH_LEN][2];
    const int np = conv_depuncture(&dp, soft, 2 * d->nblk, pairs);
    if (np < 256)
        return 1.0;
    conv_viterbi_block((const int16_t (*)[2])pairs, np, d->sbits_out, d->sdec);
    int errors = 0, count = 0;
    unsigned reg = 0;
    for (int t = 0; t < np; t++) {
        reg = (reg >> 1) | ((unsigned)d->sbits_out[t] << 6);
        if (t < 64 || t > np - 64)
            continue; /* unreliable ends */
        const int x = __builtin_parity(reg & 0171), y = __builtin_parity(reg & 0133);
        if (pairs[t][0] != 0) {
            errors += (pairs[t][0] < 0) != x;
            count++;
        }
        if (pairs[t][1] != 0) {
            errors += (pairs[t][1] < 0) != y;
            count++;
        }
    }
    return count ? (double)errors / count : 1.0;
}

static void start_stream(drx_dvbs* d, int rate, int rot90, int swap, int offset)
{
    d->rate = rate;
    d->rot90 = rot90;
    d->swap = swap;
    memset(&d->dp, 0, sizeof(d->dp));
    d->dp.p = &CONV_PUNCT[rate];
    d->dp.pos = offset;
    d->gen++;
    d->back.outer.lost = 0;
    d->state = ST_LOCK;
}

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

/* High-rate codes fit almost any bit sequence, so the raw mismatch does not
 * compare across rates. For each rate, the best hypothesis is compared with
 * the median of its other hypotheses: only the true rate stands out. */
static void search(drx_dvbs* d)
{
    double best_score = SEARCH_MAX_CONTRAST;
    int br = 0, brot = 0, bswap = 0, boff = 0;
    const int r0 = d->fixed_rate ? (int)d->fixed_rate : 1, r1 = d->fixed_rate ? (int)d->fixed_rate : 5;
    for (int rate = r0; rate <= r1; rate++) {
        double ber[2 * 2 * 8];
        int nh = 0, hrot = 0, hswap = 0, hoff = 0;
        double best = 1.0;
        for (int rot90 = 0; rot90 < 2; rot90++)
            for (int swap = 0; swap < 2; swap++)
                for (int off = 0; off < CONV_PUNCT[rate].n; off++) {
                    const double e = try_hypothesis(d, rate, rot90, swap, off);
                    ber[nh++] = e;
                    if (e < best) {
                        best = e;
                        hrot = rot90;
                        hswap = swap;
                        hoff = off;
                    }
                }
        qsort(ber, nh, sizeof(*ber), cmp_double);
        const double median = ber[nh / 2];
        const double score = median > 0 ? best / median : 1.0;
        if (best < SEARCH_MAX_BER && score < best_score) {
            best_score = score;
            br = rate;
            brot = hrot;
            bswap = hswap;
            boff = hoff;
        }
    }
    if (!br) {
        d->nblk = 0;
        if (++d->search_failures >= 3) {
            d->state = ST_ACQ;
            d->nacq = 0;
            d->search_failures = 0;
        }
        return;
    }
    d->search_failures = 0;
    start_stream(d, br, brot, bswap, boff);
}

/* Locked: depuncture into blocks for the back end. */
static void stream_symbols(drx_dvbs* d, const cf* z, int n)
{
    if (drx_tap_on(d->tap))
        drx_tap_put(d->tap, (const float*)z, n, 4);
    int16_t soft[2 * 256];
    int16_t pairs[512][2];
    /* soft_bits() as a fixed linear map of (re, im): swap is conj(z),
     * rot90 z * j (after the swap), so I = a re + b im, Q = c re + d im;
     * out of the loop, which then vectorises */
    const float ca = d->rot90 ? 0.0f : 1.0f, cb = d->rot90 ? (d->swap ? 1.0f : -1.0f) : 0.0f;
    const float cc = d->rot90 ? 1.0f : 0.0f, cd = d->rot90 ? 0.0f : (d->swap ? -1.0f : 1.0f);
    const float sc = (float)M_SQRT2 * CONV_SOFT_SCALE, mx = CONV_SOFT_MAX;
    const float* zf = (const float*)z;
    for (int i = 0; i < n; i += 256) {
        const int m = n - i < 256 ? n - i : 256;
        const float* f = zf + 2 * i;
        for (int k = 0; k < m; k++) {
            const float re = f[2 * k], im = f[2 * k + 1];
            /* comparisons, not fminf/fmaxf: those are calls without -ffinite-math-only */
            float x = (ca * re + cb * im) * sc, y = (cc * re + cd * im) * sc;
            x = x > mx ? mx : x < -mx ? -mx : x;
            y = y > mx ? mx : y < -mx ? -mx : y;
            soft[2 * k] = (int16_t)lrintf(x);
            soft[2 * k + 1] = (int16_t)lrintf(y);
        }
        int np = conv_depuncture(&d->dp, soft, 2 * m, pairs);
        for (int k = 0; k < np;) {
            if (!d->cur) {
                d->cur = queue_pop(&d->free_q, -1);
                d->cur->gen = d->gen;
                d->cur->n = 0;
            }
            int take = PB_PAIRS - d->cur->n < np - k ? PB_PAIRS - d->cur->n : np - k;
            memcpy(d->cur->pairs + d->cur->n, pairs + k, sizeof(pairs[0]) * take);
            d->cur->n += take;
            k += take;
            if (d->cur->n == PB_PAIRS)
                flush_block(d);
        }
    }
}

void drx_dvbs_process(drx_dvbs* d, const float* sym, int n, drx_dvbs_cb cb, void* ctx)
{
    d->back.outer.cb = cb;
    d->back.outer.ctx = ctx;
    const cf* s = (const cf*)sym;
    cf z[256];
    int i = 0;
    while (i < n) {
        if (d->state == ST_LOCK && d->back.outer.lost) {
            /* the back end lost the TS sync: search again, keeping the carrier */
            d->back.outer.lost = 0;
            d->state = ST_SEARCH;
            d->nblk = 0;
            d->resyncs++;
            if (d->cur) {
                queue_push(&d->free_q, d->cur);
                d->cur = NULL;
            }
        }
        if (d->state == ST_ACQ) {
            while (i < n && d->nacq < ACQ_LEN)
                d->acq[d->nacq++] = s[i++];
            if (d->nacq < ACQ_LEN)
                return;
            estimate_freq(d);
            d->state = ST_SEARCH;
            d->nblk = 0;
            /* run the PLL over the acquisition block to settle it */
            for (int k = 0; k < ACQ_LEN; k++)
                pll(d, d->acq[k]);
            d->nacq = 0;
            continue;
        }
        if (d->state == ST_SEARCH) {
            const int m = n - i < SEARCH_LEN - d->nblk ? n - i : SEARCH_LEN - d->nblk;
            pll_run(d, s + i, d->blk + d->nblk, m);
            d->nblk += m;
            i += m;
            if (d->nblk < SEARCH_LEN)
                return;
            search(d);
            if (d->state == ST_LOCK)
                stream_symbols(d, d->blk, d->nblk);
            d->nblk = 0;
            continue;
        }
        const int m = n - i < 256 ? n - i : 256;
        pll_run(d, s + i, z, m);
        i += m;
        stream_symbols(d, z, m);
    }
}

void drx_dvbs_flush(drx_dvbs* d)
{
    flush_block(d);
    while (d->threaded && d->decoded < d->pushed) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}
