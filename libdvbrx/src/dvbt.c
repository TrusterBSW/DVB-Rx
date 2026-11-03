/*
 * DVB-T (EN 300 744) receiver, see dvbt.h.
 *
 * States: DETECT folds the cyclic-prefix correlation over a few symbols for
 * each FFT size and guard interval, which gives the mode, the symbol timing
 * and the fractional carrier offset; INTCFO finds the integer offset (in
 * carriers) from the continual pilots of two consecutive symbols; TRACK
 * demodulates symbol after symbol.
 *
 * Per symbol (TRACK): the common phase error against the channel estimate
 * of the continual pilots is removed and drives the carrier loop; the
 * phase change of the continual pilots across the band between symbols
 * gives the sample clock offset, which trims the resampler; the scattered
 * pilots refresh the channel estimate on every third carrier (held for
 * four symbols), interpolated linearly in between. The TPS bits come from
 * the sign changes of the TPS carriers: their sync word gives the symbol
 * number in the frame (hence the symbol interleaver's parity), then the
 * constellation, hierarchy and code rate.
 *
 * Cells are equalised, deinterleaved (symbol, then bit-wise in blocks of
 * 126), demapped into max-log soft bits weighted by |H|^2, and go through
 * the K = 7 Viterbi decoder (puncturing phase found by re-encoding) and the
 * outer layer, on a separate thread when asked.
 */
#include "dvbrx/dvbt.h"
#include "conv.h"
#include "fft.h"
#include "outer.h"
#include "queue.h"
#include "resamp.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif

typedef float complex cf;

#define FRAME 68                /* OFDM symbols per frame */
#define DETECT_LEN 61440        /* samples folded for the mode detection */
#define DETECT_MIN 0.25f        /* normalised CP correlation needed */
#define SEARCH_SOFT 24000       /* soft bits tried per puncturing phase */
#define LOST_SYMBOLS 16         /* bad pilot correlation before restarting */
#define PILOT_MIN 0.25f         /* normalised continual pilot correlation */
#define KF 0.1                  /* carrier loop gain, per symbol */
#define CLOCK_ACQ 24            /* symbols of clock acquisition before decoding */

enum { ST_DETECT, ST_INTCFO, ST_TRACK };

/* ---- reference signal tables (EN 300 744 clause 4.5) ---------------------- */

static const short CP_2K[45] = {
    0,    48,   54,   87,   141,  156,  192,  201,  255,  279,  282,  333,  432,  450,  483,
    525,  531,  618,  636,  714,  759,  765,  780,  804,  873,  888,  918,  939,  942,  969,
    984,  1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
};
static const short CP_8K[177] = {
    0,    48,   54,   87,   141,  156,  192,  201,  255,  279,  282,  333,  432,  450,  483,
    525,  531,  618,  636,  714,  759,  765,  780,  804,  873,  888,  918,  939,  942,  969,
    984,  1050, 1101, 1107, 1110, 1137, 1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704,
    1752, 1758, 1791, 1845, 1860, 1896, 1905, 1959, 1983, 1986, 2037, 2136, 2154, 2187, 2229,
    2235, 2322, 2340, 2418, 2463, 2469, 2484, 2508, 2577, 2592, 2622, 2643, 2646, 2673, 2688,
    2754, 2805, 2811, 2814, 2841, 2844, 2850, 2910, 2973, 3027, 3081, 3195, 3387, 3408, 3456,
    3462, 3495, 3549, 3564, 3600, 3609, 3663, 3687, 3690, 3741, 3840, 3858, 3891, 3933, 3939,
    4026, 4044, 4122, 4167, 4173, 4188, 4212, 4281, 4296, 4326, 4347, 4350, 4377, 4392, 4458,
    4509, 4515, 4518, 4545, 4548, 4554, 4614, 4677, 4731, 4785, 4899, 5091, 5112, 5160, 5166,
    5199, 5253, 5268, 5304, 5313, 5367, 5391, 5394, 5445, 5544, 5562, 5595, 5637, 5643, 5730,
    5748, 5826, 5871, 5877, 5892, 5916, 5985, 6000, 6030, 6051, 6054, 6081, 6096, 6162, 6213,
    6219, 6222, 6249, 6252, 6258, 6318, 6381, 6435, 6489, 6603, 6795, 6816,
};
static const short TPS_2K[17] = {
    34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687,
};
static const short TPS_8K[68] = {
    34,   50,   209,  346,  413,  569,  595,  688,  790,  901,  1073, 1219, 1262, 1286,
    1469, 1594, 1687, 1738, 1754, 1913, 2050, 2117, 2273, 2299, 2392, 2494, 2605, 2777,
    2923, 2966, 2990, 3173, 3298, 3391, 3442, 3458, 3617, 3754, 3821, 3977, 4003, 4096,
    4198, 4309, 4481, 4627, 4670, 4694, 4877, 5002, 5095, 5146, 5162, 5321, 5458, 5525,
    5681, 5707, 5800, 5902, 6013, 6185, 6331, 6374, 6398, 6581, 6706, 6799,
};

/* TPS sync word s1..s16 of frames 1 and 3; frames 2 and 4 send it inverted */
#define TPS_SYNC 0x35EE

/* bit-wise interleavers: H_e(w) = (w + offset) mod 126 */
static const int BIT_OFFSET[6] = { 0, 63, 105, 42, 21, 84 };
/* demultiplexer, non-hierarchical: input bit k of a group goes to stream */
static const int DEMUX2[2] = { 0, 1 }, DEMUX4[4] = { 0, 2, 1, 3 }, DEMUX6[6] = { 0, 2, 4, 1, 3, 5 };

struct mode {
    int n, kmax, kc, ndata, ncp, ntps;
    const short *cp, *tps;
    float* pref;      /* pilot value of each carrier: 4/3 * (1 - 2 w_k) */
    int* data[4];     /* data carriers by scattered pilot phase */
    int* hq;          /* symbol interleaver H(q) */
    int* hinv;        /* its inverse */
};

static void free_mode(struct mode* m)
{
    free(m->pref);
    for (int p = 0; p < 4; p++)
        free(m->data[p]);
    free(m->hq);
    free(m->hinv);
    memset(m, 0, sizeof(*m));
}

static int make_mode(struct mode* m, int n)
{
    memset(m, 0, sizeof(*m));
    m->n = n;
    m->kmax = n == 2048 ? 1704 : 6816;
    m->kc = m->kmax / 2;
    m->ndata = n == 2048 ? 1512 : 6048;
    m->cp = n == 2048 ? CP_2K : CP_8K;
    m->ncp = n == 2048 ? 45 : 177;
    m->tps = n == 2048 ? TPS_2K : TPS_8K;
    m->ntps = n == 2048 ? 17 : 68;
    const int nk = m->kmax + 1;
    m->pref = malloc(sizeof(float) * nk);
    m->hq = malloc(sizeof(int) * n);
    m->hinv = malloc(sizeof(int) * n);
    uint8_t* special = calloc(nk, 1);
    int ok = m->pref && m->hq && m->hinv && special;
    for (int p = 0; p < 4 && ok; p++)
        ok = (m->data[p] = malloc(sizeof(int) * m->ndata)) != NULL;
    if (!ok) {
        free(special);
        free_mode(m);
        return -1;
    }
    /* w_k: x^11 + x^2 + 1, all ones at k = 0 */
    unsigned reg = 0x7FF;
    for (int k = 0; k < nk; k++) {
        m->pref[k] = (reg & 1) ? -4.0f / 3 : 4.0f / 3;
        const unsigned b = ((reg >> 2) ^ reg) & 1;
        reg = (reg >> 1) | (b << 10);
    }
    for (int i = 0; i < m->ncp; i++)
        special[m->cp[i]] = 1;
    for (int i = 0; i < m->ntps; i++)
        special[m->tps[i]] = 1;
    for (int p = 0; p < 4; p++) {
        int q = 0;
        for (int k = 0; k < nk; k++)
            if (!special[k] && (k % 12) != 3 * p && q < m->ndata)
                m->data[p][q++] = k;
        if (q != m->ndata)
            ok = 0;
    }
    free(special);
    /* symbol interleaver H(q) (clause 4.3.4.2) */
    static const char PERM_2K[10] = { 4, 3, 9, 6, 2, 8, 1, 5, 7, 0 };
    static const char PERM_8K[12] = { 7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5 };
    const char* perm = n == 2048 ? PERM_2K : PERM_8K;
    const int nr = n == 2048 ? 11 : 13;
    int q = 0, r = 0;
    for (int i = 0; i < n && q < m->ndata; i++) {
        if (i == 2)
            r = 1;
        else if (i > 2) {
            const int bit = n == 2048 ? (r ^ (r >> 3)) & 1 : (r ^ (r >> 1) ^ (r >> 4) ^ (r >> 6)) & 1;
            r = ((r >> 1) | (bit << (nr - 2))) & ((1 << nr) - 1);
        }
        int rp = 0;
        for (int b = 0; b < nr - 1; b++)
            rp |= ((r >> b) & 1) << perm[b];
        const int h = ((i % 2) << (nr - 1)) + rp;
        if (h < m->ndata)
            m->hq[q++] = h;
    }
    if (!ok || q != m->ndata) {
        free_mode(m);
        return -1;
    }
    for (int i = 0; i < m->ndata; i++)
        m->hinv[m->hq[i]] = i;
    return 0;
}

/* ---- constellations (clause 4.3.5) ----------------------------------------- */

/* One axis: signed levels with their bits (sign, then amplitude bits from
 * the most significant one), in the units where the closest levels of the
 * non-hierarchical constellation are 2 apart. */
struct axis {
    int nl, nb;       /* levels, bits per axis */
    float level[8];
    uint8_t bits[8];  /* bit j of the axis in bit (nb - 1 - j) */
    float norm;       /* constellation scale: unit-energy amplitude * norm = units */
    float shift;      /* alpha - 1: amplitudes are shift + 1, 3, 5, 7 */
};

static void make_axis(struct axis* a, int qam, int alpha)
{
    memset(a, 0, sizeof(*a));
    a->nb = qam == 4 ? 1 : qam == 16 ? 2 : 3;
    const int na = 1 << (a->nb - 1); /* amplitudes per sign */
    const int al = alpha ? alpha : 1;
    a->shift = (float)(al - 1);
    double e = 0;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < na; i++) {
            /* amplitude bits g (Gray): 0 is the outermost level */
            const int g = i ^ (i >> 1);
            const float amp = (float)(al + 2 * (na - 1 - i));
            a->level[a->nl] = s ? -amp : amp;
            a->bits[a->nl] = (uint8_t)((s << (a->nb - 1)) | g);
            a->nl++;
            e += amp * amp;
        }
    /* both axes: E = 2 E[amp^2] */
    a->norm = (float)sqrt(2 * e / a->nl);
}

/* Max-log soft values of one axis, positive for a 0 bit, (d1 - d0) / 4 in
 * level units: for Gray PAM they are piecewise linear around the decision
 * thresholds (sign at 0, then the amplitude thresholds at 2 or 4, and 2 on
 * either side of 4). */
static inline void axis_llr(const struct axis* a, float r, float* llr)
{
    llr[0] = r;
    if (a->nb == 1)
        return;
    const float u = fabsf(r) - a->shift;
    if (a->nb == 2) {
        llr[1] = u - 2;
        return;
    }
    llr[1] = u - 4;
    llr[2] = fabsf(u - 4) - 2;
}

/* distance to the nearest level */
static inline float axis_error(const struct axis* a, float r)
{
    const int na = a->nl / 2;
    float u = fabsf(r) - a->shift;
    float l = 2 * floorf(u * 0.5f) + 1;
    l = l < 1 ? 1 : l > 2 * na - 1 ? (float)(2 * na - 1) : l;
    return u - l;
}

/* ---- back end: Viterbi and outer layer ------------------------------------ */

#define PB_PAIRS 8192
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

static void bits_out(void* ctx, const uint8_t* bits, int n) { outer_bits(ctx, bits, n); }

/* ---- receiver --------------------------------------------------------------- */

struct drx_dvbt {
    double in_rate, elem_rate;
    int threaded;
    struct resamp rs;
    double step_nominal;  /* input / elementary rate */
    _Atomic double step;  /* resampler step to use, set by the demodulator */
    double cur_step;      /* step the samples being demodulated were made with */
    double clock;         /* transmitter clock estimate, step / nominal - 1 */

    cf* buf;  /* resampled samples */
    int nbuf, cap, pos;

    int state;
    struct mode m;
    struct fft fft;
    int g, nsym_len;
    double f, phase; /* NCO: cycles/sample, phase at buf[pos] */
    double tau;      /* accumulated timing drift, samples */
    double drift;    /* smoothed timing drift, samples per symbol */
    int tracked;     /* symbols since INTCFO */
    int no_tps;      /* symbols without TPS frame sync */
    int outer_fails; /* packet sync searches failed in a row */
    uint64_t packets_seen;
    int settled;     /* the clock loop has converged: decode */
    cf* win;         /* FFT buffer */
    cf* x;           /* carriers of the current symbol */
    cf* prev;        /* previous symbol, for INTCFO and TPS */
    cf* cp_prev;     /* continual pilots of the previous symbol, derotated */
    int have_prev;
    float* intcfo_metric;

    /* channel estimate on carriers 3i, per scattered phase filled */
    cf* h3;
    int h3_fill; /* bit p: phase p seen since the last reset */
    int sp;      /* scattered pilot phase of the current symbol, -1 unknown */
    int bad;     /* symbols with a poor pilot correlation */

    /* TPS */
    uint32_t tps_sync;  /* last differential bits */
    uint8_t tps[FRAME];
    int l;              /* symbol number in the frame, -1 unknown */
    int params;         /* TPS decoded */
    int qam, alpha, rate_hp, rate_lp, guard_tps, mode_tps;
    struct axis ax;
    int v;              /* bits per cell decoded (HP only when hierarchical) */

    /* demodulation */
    cf* hk;                  /* channel on every carrier, interpolated */
    int16_t (*cell_soft)[8]; /* per cell, after symbol deinterleaving: soft bits */
    int16_t* soft;           /* serial soft bits of one symbol */
    float mer_err, mer_sig;
    float hmean;
    cf slope_back, slope1, slope2; /* e^(-j phi), e^(j phi / 3), e^(j 2 phi / 3) */

    /* inner decoding */
    int16_t* search; /* soft bits kept for the puncturing phase search */
    int nsearch;
    int streaming;
    struct conv_depunct dp;
    struct pblock* cur;
    int gen;

    struct back back;
    pthread_t thread;
    struct queue free_q, full_q;
    uint64_t pushed;
    _Atomic uint64_t decoded;

    uint64_t symbols, resyncs;
};

static void* back_thread(void* arg)
{
    drx_dvbt* d = arg;
    struct pblock* p;
    while ((p = queue_pop(&d->full_q, -1))) {
        if (p->gen != d->back.gen) {
            d->back.gen = p->gen;
            conv_viterbi_reset(&d->back.vit);
            outer_reset(&d->back.outer);
        }
        if (d->back.outer.active)
            conv_viterbi_decode(&d->back.vit, (const int16_t (*)[2])p->pairs, p->n, bits_out,
                                &d->back.outer, &d->back.outer.active);
        queue_push(&d->free_q, p);
        d->decoded++;
    }
    return NULL;
}

static void flush_block(drx_dvbt* d)
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
    if (p->gen != d->back.gen) {
        d->back.gen = p->gen;
        conv_viterbi_reset(&d->back.vit);
        outer_reset(&d->back.outer);
    }
    if (d->back.outer.active)
        conv_viterbi_decode(&d->back.vit, (const int16_t (*)[2])p->pairs, p->n, bits_out,
                            &d->back.outer, &d->back.outer.active);
    queue_push(&d->free_q, p);
}

drx_dvbt* drx_dvbt_new(double sample_rate, double bandwidth, int threaded)
{
    if (bandwidth < 1e6 || sample_rate < bandwidth)
        return NULL;
    drx_dvbt* d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->in_rate = sample_rate;
    d->elem_rate = bandwidth * 8.0 / 7.0;
    d->threaded = threaded;
    d->step_nominal = sample_rate / d->elem_rate;
    d->step = d->step_nominal;
    d->clock = 0;
    d->cap = 1 << 18;
    d->buf = malloc(sizeof(cf) * d->cap);
    d->win = malloc(sizeof(cf) * 8192);
    d->x = malloc(sizeof(cf) * 6817);
    d->prev = malloc(sizeof(cf) * 8192); /* carriers, or FFT bins in INTCFO */
    d->cp_prev = malloc(sizeof(cf) * 177);
    d->intcfo_metric = malloc(sizeof(float) * 1025);
    d->h3 = malloc(sizeof(cf) * (6816 / 3 + 2));
    d->hk = malloc(sizeof(cf) * (6816 + 3));
    d->cell_soft = malloc(sizeof(*d->cell_soft) * 6048);
    d->soft = malloc(sizeof(int16_t) * 6048 * 6);
    d->search = malloc(sizeof(int16_t) * (SEARCH_SOFT + 6048 * 6));
    int ok = d->buf && d->win && d->x && d->prev && d->cp_prev && d->intcfo_metric && d->h3 &&
             d->hk && d->cell_soft && d->soft && d->search;
    /* the useful band: 7.61 MHz of the 9.14 MHz elementary rate (8 MHz) */
    ok = ok && !resamp_init(&d->rs, d->step_nominal, 0.86);
    ok = ok && !conv_viterbi_init(&d->back.vit) && !outer_init(&d->back.outer);
    d->back.outer.active = 0;
    d->back.gen = -1;
    ok = ok && !queue_init(&d->free_q, NPB) && !queue_init(&d->full_q, NPB);
    for (int i = 0; ok && i < NPB; i++) {
        struct pblock* p = malloc(sizeof(*p));
        ok = p != NULL;
        if (ok)
            queue_push(&d->free_q, p);
    }
    if (!ok) {
        drx_dvbt_free(d);
        return NULL;
    }
    d->state = ST_DETECT;
    if (threaded)
        pthread_create(&d->thread, NULL, back_thread, d);
    return d;
}

void drx_dvbt_free(drx_dvbt* d)
{
    if (!d)
        return;
    if (d->threaded) {
        flush_block(d);
        queue_close(&d->full_q);
        pthread_join(d->thread, NULL);
    }
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
    conv_viterbi_free(&d->back.vit);
    outer_free(&d->back.outer);
    resamp_free(&d->rs);
    fft_free(&d->fft);
    free_mode(&d->m);
    free(d->buf);
    free(d->win);
    free(d->x);
    free(d->prev);
    free(d->cp_prev);
    free(d->intcfo_metric);
    free(d->h3);
    free(d->hk);
    free(d->cell_soft);
    free(d->soft);
    free(d->search);
    free(d);
}

void drx_dvbt_get_stats(const drx_dvbt* d, drx_dvbt_stats* s)
{
    memset(s, 0, sizeof(*s));
    s->locked = d->state == ST_TRACK && d->streaming && d->back.outer.synced;
    s->fft = d->state == ST_TRACK ? d->m.n : 0;
    s->guard = d->state == ST_TRACK && d->g ? d->m.n / d->g : 0;
    s->qam = d->params ? d->qam : 0;
    s->rate = d->params ? d->rate_hp : 0;
    s->hierarchy = d->params ? d->alpha : 0;
    s->snr_db = 10.0f * log10f((d->mer_sig + 1e-20f) / (d->mer_err + 1e-20f));
    s->freq = (float)(d->f / d->step_nominal);
    s->clock_ppm = (float)(d->clock * 1e6);
    s->symbols = d->symbols;
    s->packets = d->back.outer.packets;
    s->rs_corrected = d->back.outer.rs_corrected;
    s->rs_failed = d->back.outer.rs_failed;
    s->resyncs = d->resyncs;
}

/* ---- acquisition ---------------------------------------------------------- */

static void restart(drx_dvbt* d)
{
    d->state = ST_DETECT;
    d->resyncs++;
    d->params = 0;
    d->streaming = 0;
    d->nsearch = 0;
    d->gen++;
    if (d->cur) {
        queue_push(&d->free_q, d->cur);
        d->cur = NULL;
    }
}

/* Cyclic prefix correlation folded over the symbols of the block, for each
 * mode and guard interval. */
static int detect(drx_dvbt* d)
{
    const cf* x = d->buf + d->pos;
    const int L = DETECT_LEN;
    double complex* C = malloc(sizeof(*C) * (L + 1));
    double* E = malloc(sizeof(*E) * (L + 1));
    double complex* fold = malloc(sizeof(*fold) * 10240);
    double* efold = malloc(sizeof(*efold) * 10240);
    if (!C || !E || !fold || !efold) {
        free(C);
        free(E);
        free(fold);
        free(efold);
        return -1;
    }
    float best = 0;
    int bn = 0, bg = 0, brho = 0;
    double bf = 0;
    static const int NS[2] = { 2048, 8192 };
    for (int mi = 0; mi < 2; mi++) {
        const int n = NS[mi];
        const int m = L - n;
        C[0] = 0;
        E[0] = 0;
        for (int i = 0; i < m; i++) {
            C[i + 1] = C[i] + x[i] * conjf(x[i + n]);
            E[i + 1] = E[i] + 0.5 * (crealf(x[i] * conjf(x[i])) + crealf(x[i + n] * conjf(x[i + n])));
        }
        for (int gd = 4; gd <= 32; gd *= 2) {
            const int g = n / gd, ls = n + g;
            const int nwin = m - g;
            if (nwin < 2 * ls)
                continue;
            for (int r = 0; r < ls; r++) {
                fold[r] = 0;
                efold[r] = 0;
            }
            for (int i = 0; i < nwin; i++) {
                fold[i % ls] += C[i + g] - C[i];
                efold[i % ls] += E[i + g] - E[i];
            }
            for (int r = 0; r < ls; r++) {
                const float v = (float)(cabs(fold[r]) / (efold[r] + 1e-30));
                if (v > best) {
                    best = v;
                    bn = n;
                    bg = g;
                    brho = r;
                    bf = -carg(fold[r]) / (2 * M_PI * n);
                }
            }
        }
    }
    free(C);
    free(E);
    free(fold);
    free(efold);
    if (best < DETECT_MIN)
        return 0;
    if (d->m.n != bn) {
        free_mode(&d->m);
        fft_free(&d->fft);
        if (make_mode(&d->m, bn) || fft_init(&d->fft, bn))
            return -1;
    }
    d->g = bg;
    d->nsym_len = bn + bg;
    d->f = bf;
    d->phase = 0;
    /* FFT window: a quarter of the guard interval before the useful part */
    d->pos += brho + bg - bg / 4;
    return 1;
}

/* FFT of the window at pos, NCO applied; carriers k = 0..kmax into d->x. */
static void demod_window(drx_dvbt* d)
{
    const int n = d->m.n;
    const cf* in = d->buf + d->pos;
    const double w = -2 * M_PI * d->f;
    const cf stepr = (cf)cexp(I * w);
    /* The NCO restarts from cexp every 256 samples, then turns by stepr: the
     * chunks are independent chains (of dependent complex products), run
     * four side by side (not more: the chunks are 2 KB apart, the same L1
     * sets). The products are spelled out as the compiler contracts win =
     * in * rot, rot *= stepr into FMAs. */
    const int nc = n / 256;
    float rr[32], ri[32];
    for (int c = 0; c < nc; c++) {
        const int i = 256 * c;
        const cf rot = (cf)cexp(I * (-d->phase + w * i));
        rr[c] = crealf(rot);
        ri[c] = cimagf(rot);
    }
    const float sr = crealf(stepr), si = cimagf(stepr);
    const float* inf = (const float*)in;
    float* wf = (float*)d->win;
    for (int c0 = 0; c0 < nc; c0 += 4)
        for (int i = 0; i < 256; i++)
            for (int c = c0; c < c0 + 4; c++) {
                const int t = 2 * (256 * c + i);
                const float a = inf[t], b = inf[t + 1], r = rr[c], q = ri[c];
                wf[t] = fmaf(r, a, -(q * b));
                wf[t + 1] = fmaf(q, a, r * b);
                rr[c] = fmaf(sr, r, -(si * q));
                ri[c] = fmaf(si, r, sr * q);
            }
    fft_run(&d->fft, d->win, 0);
    /* carriers k = 0..kmax from bins k - kc (mod n) */
    const float scale = 1.0f / sqrtf((float)n);
    const int kc = d->m.kc;
    for (int k = 0; k < kc; k++)
        d->x[k] = d->win[k - kc + n] * scale;
    for (int k = kc; k <= d->m.kmax; k++)
        d->x[k] = d->win[k - kc] * scale;
}

static void advance(drx_dvbt* d, int samples)
{
    d->pos += samples;
    d->phase = fmod(d->phase + 2 * M_PI * d->f * samples, 2 * M_PI);
}

/* Integer carrier offset: the continual pilots stay put from symbol to
 * symbol, the data does not. */
static void intcfo(drx_dvbt* d)
{
    const int n = d->m.n, imax = n / 16;
    float best = -1;
    int bi = 0;
    for (int off = -imax; off <= imax; off++) {
        cf s = 0;
        for (int i = 0; i < d->m.ncp; i++) {
            const int b = (d->m.cp[i] - d->m.kc + off + 2 * n) % n;
            s += d->win[b] * conjf(d->prev[b]);
        }
        const float v = cabsf(s);
        if (v > best) {
            best = v;
            bi = off;
        }
    }
    d->f += (double)bi / n;
}

/* ---- TPS -------------------------------------------------------------------- */

static int bits_of(const uint8_t* s, int from, int to)
{
    int v = 0;
    for (int i = from; i <= to; i++)
        v = v << 1 | s[i];
    return v;
}

static void parse_tps(drx_dvbt* d)
{
    const uint8_t* s = d->tps;
    const int li = bits_of(s, 17, 22);
    if (li != 23 && li != 31)
        return;
    static const int QAM[4] = { 4, 16, 64, 0 };
    static const int ALPHA[8] = { 0, 1, 2, 4, -1, -1, -1, -1 };
    const int qam = QAM[bits_of(s, 25, 26)], alpha = ALPHA[bits_of(s, 27, 29)];
    const int hp = bits_of(s, 30, 32) + 1, lp = bits_of(s, 33, 35) + 1;
    if (!qam || alpha < 0 || hp > 5)
        return;
    if (d->params && (qam != d->qam || alpha != d->alpha || hp != d->rate_hp)) {
        /* the transmitter changed its parameters: start the inner decoding over */
        d->streaming = 0;
        d->nsearch = 0;
        d->gen++;
    }
    d->qam = qam;
    d->alpha = alpha;
    d->rate_hp = hp;
    d->rate_lp = lp;
    d->guard_tps = 32 >> bits_of(s, 36, 37);
    d->mode_tps = bits_of(s, 38, 39);
    make_axis(&d->ax, qam, alpha);
    d->v = alpha ? 2 : (qam == 4 ? 2 : qam == 16 ? 4 : 6);
    d->params = 1;
}

/* ---- demodulation of one symbol --------------------------------------------- */

/* Channel at carrier k, from the estimates on every third carrier: linear
 * interpolation once the mean phase slope across the band (the window
 * sits ahead of the useful part, and the channel delays it) is taken out. */
static inline cf chan(const drx_dvbt* d, int k)
{
    const int i = k / 3, r = k - 3 * i;
    if (!r)
        return d->h3[i];
    const cf next = d->h3[i + 1] * d->slope_back;
    return r == 1 ? (d->h3[i] * (2.0f / 3) + next * (1.0f / 3)) * d->slope1
                  : (d->h3[i] * (1.0f / 3) + next * (2.0f / 3)) * d->slope2;
}

/* Channel on every carrier: chan() of each, without its division and
 * branches in the per-cell loop. The products are spelled out as the
 * compiler contracts chan() into FMAs, so that the soft bits stay exactly
 * the same. */
static void interp_channel(drx_dvbt* d)
{
    const int kmax = d->m.kmax; /* a multiple of 3 */
    cf* hk = d->hk;
#ifdef __FMA__
    const float sbr = crealf(d->slope_back), sbi = cimagf(d->slope_back);
    const float s1r = crealf(d->slope1), s1i = cimagf(d->slope1);
    const float s2r = crealf(d->slope2), s2i = cimagf(d->slope2);
    float* o = (float*)hk;
    const float* h3 = (const float*)d->h3;
    for (int i = 0; 3 * i < kmax; i++) {
        const float hr = h3[2 * i], hi = h3[2 * i + 1], gr = h3[2 * i + 2], gi = h3[2 * i + 3];
        const float nr = fmaf(gr, sbr, -(gi * sbi)), ni = fmaf(sbi, gr, gi * sbr); /* next */
        const float ar = fmaf(hr, 2.0f / 3, nr * (1.0f / 3)), ai = fmaf(hi, 2.0f / 3, ni * (1.0f / 3));
        const float br = fmaf(hr, 1.0f / 3, nr * (2.0f / 3)), bi = fmaf(hi, 1.0f / 3, ni * (2.0f / 3));
        o[6 * i] = hr;
        o[6 * i + 1] = hi;
        o[6 * i + 2] = fmaf(ar, s1r, -(s1i * ai));
        o[6 * i + 3] = fmaf(s1i, ar, ai * s1r);
        o[6 * i + 4] = fmaf(br, s2r, -(s2i * bi));
        o[6 * i + 5] = fmaf(s2i, br, bi * s2r);
    }
#else
    for (int i = 0; 3 * i < kmax; i++) {
        hk[3 * i] = d->h3[i];
        hk[3 * i + 1] = chan(d, 3 * i + 1);
        hk[3 * i + 2] = chan(d, 3 * i + 2);
    }
#endif
    hk[kmax] = d->h3[kmax / 3];
}

/* Bit deinterleaver and demultiplexer of a block of 126 cells, as a gather:
 * serial soft bit j (j < 126 v) of the block is soft bit e of cell w, at
 * GATHER[v / 2 - 1][j] = 8 w + e (cells of 8 int16). The bit-wise
 * interleavers write b_e(H_e(w)) = a_e(w), H_e(w) = (w + offset_e) mod 126,
 * and input bit k of group i is b_demux(k)(i). */
static uint16_t GATHER[3][126 * 6];
#ifdef __AVX2__
/* the same as int32 for vpgatherdd, by 16: the even j of the group, then
 * the odd ones */
static int32_t GATHER32[3][126 * 6];
#endif
static pthread_once_t gather_once = PTHREAD_ONCE_INIT;

static void make_gather(void)
{
    for (int vi = 0; vi < 3; vi++) {
        const int v = 2 * (vi + 1);
        const int* demux = v == 2 ? DEMUX2 : v == 4 ? DEMUX4 : DEMUX6;
        for (int i = 0; i < 126; i++)
            for (int k = 0; k < v; k++) {
                const int e = demux[k];
                const int w = (i - BIT_OFFSET[e] + 126) % 126;
                GATHER[vi][i * v + k] = (uint16_t)(8 * w + e);
            }
#ifdef __AVX2__
        for (int j = 0; j + 16 <= 126 * v; j += 16)
            for (int h = 0; h < 8; h++) {
                GATHER32[vi][j + h] = GATHER[vi][j + 2 * h];
                GATHER32[vi][j + 8 + h] = GATHER[vi][j + 2 * h + 1];
            }
#endif
    }
}

#ifdef __AVX2__
/* conv_soft() of 8 values, as int32 */
static inline __m256i conv_soft8(__m256 v)
{
    v = _mm256_mul_ps(v, _mm256_set1_ps(CONV_SOFT_SCALE));
    /* minps(a, b) = a < b ? a : b: the order of conv_soft's tests */
    v = _mm256_min_ps(_mm256_set1_ps(CONV_SOFT_MAX), v);
    v = _mm256_max_ps(_mm256_set1_ps(-CONV_SOFT_MAX), v);
    return _mm256_cvtps_epi32(v);
}

/* The cells of the symbol, 8 at a time (ndata is a multiple of 8): the
 * same float operations as the scalar loop, lane by lane (but the MER sums
 * are made in 8 lanes, a different order); err, sig get the MER sums. */
static void demap_cells_avx2(drx_dvbt* d, const int* dk, const int* dst, float inv_hmean,
                             float* err, float* sig)
{
    const struct axis* ax = &d->ax;
    const float* x = (const float*)d->x;
    const float* hk = (const float*)d->hk;
    const __m256 norm = _mm256_set1_ps(ax->norm), shift = _mm256_set1_ps(ax->shift);
    const __m256 amax = _mm256_set1_ps((float)(ax->nl - 1)), one = _mm256_set1_ps(1);
    const __m256 two = _mm256_set1_ps(2), four = _mm256_set1_ps(4), half = _mm256_set1_ps(0.5f);
    const __m256 eps = _mm256_set1_ps(1e-12f), ihm = _mm256_set1_ps(inv_hmean);
    const __m256 absm = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256i perm = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    const __m256 nn = _mm256_set1_ps(ax->norm * ax->norm);
    __m256 merr = _mm256_setzero_ps(), msig = _mm256_setzero_ps();
    const int nb = ax->nb;
    for (int q = 0; q < d->m.ndata; q += 8) {
        /* complex x[k], h[k] of cells (q, q+1, q+4, q+5) and (q+2, q+3, q+6,
         * q+7): the even / odd floats of the pair are then re / im in order */
        const __m256i k = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(dk + q)), perm);
        const __m128i k0 = _mm256_castsi256_si128(k), k1 = _mm256_extracti128_si256(k, 1);
        const __m256 x0 = _mm256_castpd_ps(_mm256_i32gather_pd((const double*)x, k0, 8));
        const __m256 x1 = _mm256_castpd_ps(_mm256_i32gather_pd((const double*)x, k1, 8));
        const __m256 g0 = _mm256_castpd_ps(_mm256_i32gather_pd((const double*)hk, k0, 8));
        const __m256 g1 = _mm256_castpd_ps(_mm256_i32gather_pd((const double*)hk, k1, 8));
        const __m256 xr = _mm256_shuffle_ps(x0, x1, 0x88), xi = _mm256_shuffle_ps(x0, x1, 0xDD);
        const __m256 hr = _mm256_shuffle_ps(g0, g1, 0x88), hi = _mm256_shuffle_ps(g0, g1, 0xDD);
        const __m256 h2 = _mm256_fmadd_ps(hr, hr, _mm256_mul_ps(hi, hi));
        const __m256 sc = _mm256_div_ps(norm, _mm256_add_ps(h2, eps));
        const __m256 zr = _mm256_mul_ps(sc, _mm256_fmadd_ps(xr, hr, _mm256_mul_ps(hi, xi)));
        const __m256 zi = _mm256_mul_ps(sc, _mm256_fnmadd_ps(xr, hi, _mm256_mul_ps(xi, hr)));
        const __m256 csi = _mm256_mul_ps(ihm, h2);
        const __m256 ur = _mm256_sub_ps(_mm256_and_ps(zr, absm), shift);
        const __m256 ui = _mm256_sub_ps(_mm256_and_ps(zi, absm), shift);
        __m256i s[6];
        s[0] = conv_soft8(_mm256_mul_ps(csi, zr));
        s[1] = conv_soft8(_mm256_mul_ps(csi, zi));
        s[2] = s[3] = s[4] = s[5] = _mm256_setzero_si256();
        if (nb == 2) {
            s[2] = conv_soft8(_mm256_mul_ps(_mm256_sub_ps(ur, two), csi));
            s[3] = conv_soft8(_mm256_mul_ps(_mm256_sub_ps(ui, two), csi));
        } else if (nb == 3) {
            const __m256 vr = _mm256_sub_ps(ur, four), vi = _mm256_sub_ps(ui, four);
            s[2] = conv_soft8(_mm256_mul_ps(vr, csi));
            s[3] = conv_soft8(_mm256_mul_ps(vi, csi));
            s[4] = conv_soft8(_mm256_mul_ps(_mm256_sub_ps(_mm256_and_ps(vr, absm), two), csi));
            s[5] = conv_soft8(_mm256_mul_ps(_mm256_sub_ps(_mm256_and_ps(vi, absm), two), csi));
        }
        /* MER terms, summed in order afterwards */
        __m256 lr = _mm256_fmadd_ps(_mm256_floor_ps(_mm256_mul_ps(ur, half)), two, one);
        __m256 li = _mm256_fmadd_ps(_mm256_floor_ps(_mm256_mul_ps(ui, half)), two, one);
        lr = _mm256_min_ps(_mm256_max_ps(lr, one), amax);
        li = _mm256_min_ps(_mm256_max_ps(li, one), amax);
        const __m256 er = _mm256_sub_ps(ur, lr), ei = _mm256_sub_ps(ui, li);
        merr = _mm256_fmadd_ps(_mm256_fmadd_ps(er, er, _mm256_mul_ps(ei, ei)), h2, merr);
        msig = _mm256_fmadd_ps(nn, h2, msig);
        /* 8 x 8 int16 transpose: one row (soft bits 0..5, 2 unused) a cell */
        const __m256i a01 = _mm256_packs_epi32(s[0], s[1]), a23 = _mm256_packs_epi32(s[2], s[3]);
        const __m256i a45 = _mm256_packs_epi32(s[4], s[5]), z = _mm256_setzero_si256();
        const __m256i u0 = _mm256_unpacklo_epi16(a01, a23), u1 = _mm256_unpackhi_epi16(a01, a23);
        const __m256i v0 = _mm256_unpacklo_epi16(a45, z), v1 = _mm256_unpackhi_epi16(a45, z);
        const __m256i w0 = _mm256_unpacklo_epi16(u0, u1), w1 = _mm256_unpackhi_epi16(u0, u1);
        const __m256i y0 = _mm256_unpacklo_epi16(v0, v1), y1 = _mm256_unpackhi_epi16(v0, v1);
        const __m256i r0 = _mm256_unpacklo_epi64(w0, y0), r1 = _mm256_unpackhi_epi64(w0, y0);
        const __m256i r2 = _mm256_unpacklo_epi64(w1, y1), r3 = _mm256_unpackhi_epi64(w1, y1);
        int16_t (*cs)[8] = d->cell_soft;
        _mm_storeu_si128((__m128i*)cs[dst[q]], _mm256_castsi256_si128(r0));
        _mm_storeu_si128((__m128i*)cs[dst[q + 1]], _mm256_castsi256_si128(r1));
        _mm_storeu_si128((__m128i*)cs[dst[q + 2]], _mm256_castsi256_si128(r2));
        _mm_storeu_si128((__m128i*)cs[dst[q + 3]], _mm256_castsi256_si128(r3));
        _mm_storeu_si128((__m128i*)cs[dst[q + 4]], _mm256_extracti128_si256(r0, 1));
        _mm_storeu_si128((__m128i*)cs[dst[q + 5]], _mm256_extracti128_si256(r1, 1));
        _mm_storeu_si128((__m128i*)cs[dst[q + 6]], _mm256_extracti128_si256(r2, 1));
        _mm_storeu_si128((__m128i*)cs[dst[q + 7]], _mm256_extracti128_si256(r3, 1));
    }
    float t[16];
    _mm256_storeu_ps(t, merr);
    _mm256_storeu_ps(t + 8, msig);
    *err = ((t[0] + t[1]) + (t[2] + t[3])) + ((t[4] + t[5]) + (t[6] + t[7]));
    *sig = ((t[8] + t[9]) + (t[10] + t[11])) + ((t[12] + t[13]) + (t[14] + t[15]));
}

/* Bit deinterleaver and demultiplexer of the symbol (see GATHER), 16 soft
 * bits a gather pair; returns how many. */
static int gather_bits_avx2(drx_dvbt* d)
{
    const int v = d->v, n = 126 * v;
    const int32_t* g32 = GATHER32[v / 2 - 1];
    const uint16_t* g = GATHER[v / 2 - 1];
    int16_t* out = d->soft;
    for (int blk = 0; blk < d->m.ndata / 126; blk++) {
        const int16_t* a = d->cell_soft[blk * 126];
        int j = 0;
        for (; j + 16 <= n; j += 16) {
            const __m256i ie = _mm256_loadu_si256((const __m256i*)(g32 + j));
            const __m256i io = _mm256_loadu_si256((const __m256i*)(g32 + j + 8));
            /* 32 bits at each soft bit: it in the low half */
            const __m256i ve = _mm256_i32gather_epi32((const int*)a, ie, 2);
            const __m256i vo = _mm256_i32gather_epi32((const int*)a, io, 2);
            const __m256i r = _mm256_blend_epi16(ve, _mm256_slli_epi32(vo, 16), 0xAA);
            _mm256_storeu_si256((__m256i*)(out + j), r);
        }
        for (; j < n; j++)
            out[j] = a[g[j]];
        out += n;
    }
    return (int)(out - d->soft);
}
#endif

/* Soft bits of the current symbol (d->x, equalisation done here), in the
 * serial order of the inner coder output, into d->soft; returns how many.
 * (Not inlined: inlined into run(), it changes how the compiler contracts
 * the floating point of track_symbol, hence its results in the last bits.) */
__attribute__((noinline)) static int demap_symbol(drx_dvbt* d)
{
    const struct mode* m = &d->m;
    const int* dk = m->data[d->l & 3];
    const int odd = d->l & 1;
    const struct axis* ax = &d->ax;
    const float inv_hmean = 1.0f / (d->hmean + 1e-12f);
    const float norm = ax->norm;
    float err = 0, sig = 0;
    interp_channel(d);
    /* symbol deinterleaver: y'(H(q)) = y(q) on odd symbols, y'(q) = y(H(q))
     * on even ones (clause 4.3.4.2) */
    const int* dst = odd ? m->hq : m->hinv;
#ifdef __AVX2__
    (void)norm;
    demap_cells_avx2(d, dk, dst, inv_hmean, &err, &sig);
#else
    const float shift = ax->shift, amax = (float)(ax->nl - 1);
    for (int q = 0; q < m->ndata; q++) {
        const int k = dk[q];
        const float hr = crealf(d->hk[k]), hi = cimagf(d->hk[k]);
        const float xr = crealf(d->x[k]), xi = cimagf(d->x[k]);
        /* the same operations (and fused multiply-adds) as the compiler made
         * of h2 = |h|^2, z = x conj(h) norm / (h2 + 1e-12): one division */
        const float h2 = fmaf(hr, hr, hi * hi);
        const float sc = norm / (h2 + 1e-12f);
        const float zr = sc * fmaf(xr, hr, hi * xi), zi = sc * fmaf(-xr, hi, xi * hr);
        const float csi = inv_hmean * h2;
        int16_t* out = d->cell_soft[dst[q]];
        /* max-log soft bits (see axis_llr): bits y0 y1 the signs (I, Q),
         * then I and Q amplitude bits alternately */
        out[0] = conv_soft(csi * zr);
        out[1] = conv_soft(csi * zi);
        const float ur = fabsf(zr) - shift, ui = fabsf(zi) - shift;
        if (ax->nb == 2) {
            out[2] = conv_soft((ur - 2) * csi);
            out[3] = conv_soft((ui - 2) * csi);
        } else if (ax->nb == 3) {
            out[2] = conv_soft((ur - 4) * csi);
            out[3] = conv_soft((ui - 4) * csi);
            out[4] = conv_soft((fabsf(ur - 4) - 2) * csi);
            out[5] = conv_soft((fabsf(ui - 4) - 2) * csi);
        }
        /* MER: distance to the nearest level (axis_error) */
        float lr = fmaf(floorf(ur * 0.5f), 2, 1), li = fmaf(floorf(ui * 0.5f), 2, 1);
        lr = lr < 1 ? 1 : lr > amax ? amax : lr;
        li = li < 1 ? 1 : li > amax ? amax : li;
        const float er = ur - lr, ei = ui - li;
        err = fmaf(fmaf(er, er, ei * ei), h2, err);
        sig = fmaf(norm * norm, h2, sig);
    }
#endif
    d->mer_err += 0.05f * (err - d->mer_err);
    d->mer_sig += 0.05f * (sig - d->mer_sig);

    pthread_once(&gather_once, make_gather);
#ifdef __AVX2__
    return gather_bits_avx2(d);
#else
    const int v = d->v;
    const uint16_t* g = GATHER[v / 2 - 1];
    int ns = 0;
    for (int blk = 0; blk < m->ndata / 126; blk++) {
        const int16_t* a = d->cell_soft[blk * 126];
        for (int j = 0; j < 126 * v; j++)
            d->soft[ns++] = a[g[j]];
    }
    return ns;
#endif
}

static void push_pairs(drx_dvbt* d, const int16_t* soft, int n)
{
    int16_t pairs[1024][2];
    for (int i = 0; i < n; i += 512) {
        const int m = n - i < 512 ? n - i : 512;
        const int np = conv_depuncture(&d->dp, soft + i, m, pairs);
        for (int k = 0; k < np;) {
            if (!d->cur) {
                d->cur = queue_pop(&d->free_q, -1);
                d->cur->gen = d->gen;
                d->cur->n = 0;
            }
            const int take = PB_PAIRS - d->cur->n < np - k ? PB_PAIRS - d->cur->n : np - k;
            memcpy(d->cur->pairs + d->cur->n, pairs + k, sizeof(pairs[0]) * take);
            d->cur->n += take;
            k += take;
            if (d->cur->n == PB_PAIRS)
                flush_block(d);
        }
    }
}

/* Inner decoding of a symbol's soft bits: first the puncturing phase. */
static void inner(drx_dvbt* d, const int16_t* soft, int n)
{
    if (d->streaming) {
        push_pairs(d, soft, n);
        return;
    }
    memcpy(d->search + d->nsearch, soft, sizeof(*soft) * n);
    d->nsearch += n;
    if (d->nsearch < SEARCH_SOFT)
        return;
    const struct conv_punct* p = &CONV_PUNCT[d->rate_hp];
    double best = 1;
    int bph = 0;
    for (int ph = 0; ph < p->n; ph++) {
        const double e = conv_mismatch(d->search, SEARCH_SOFT, d->rate_hp, ph);
        if (e < best) {
            best = e;
            bph = ph;
        }
    }
    memset(&d->dp, 0, sizeof(d->dp));
    d->dp.p = p;
    d->dp.pos = bph;
    d->streaming = 1;
    d->gen++;
    d->back.outer.lost = 0;
    push_pairs(d, d->search, d->nsearch);
    d->nsearch = 0;
}

/* h3[i] *= c3, c3 *= r3 (i = 0..nh-1), and when prev is given, prev[k] *=
 * c1, c1 *= r1 (k = 0..kmax): two independent chains of complex products
 * (each step needs the previous phasor), run side by side, a step of the h3
 * one every three of the prev one. The products are spelled out as the
 * compiler contracts x *= c, c *= r into FMAs (GCC 14 and 16 alike). */
__attribute__((noinline)) static void turn_chains(cf* h3, int nh, cf* prev, int kmax, cf c0, cf r3,
                                                  cf r1)
{
#define TURN(x, cr, ci)                                                                            \
    do {                                                                                           \
        const float a_ = (x)[0], b_ = (x)[1];                                                      \
        (x)[0] = fmaf(cr, a_, -(ci * b_));                                                         \
        (x)[1] = fmaf(ci, a_, cr * b_);                                                            \
    } while (0)
#define STEP(cr, ci, rr, ri)                                                                       \
    do {                                                                                           \
        const float t_ = fmaf(cr, rr, -(ci * ri));                                                 \
        ci = fmaf(cr, ri, ci * rr);                                                                \
        cr = t_;                                                                                   \
    } while (0)
    float* h = (float*)h3;
    float hr = crealf(c0), hi = cimagf(c0);
    const float r3r = crealf(r3), r3i = cimagf(r3);
    int i = 0;
    if (prev) {
        float* p = (float*)prev;
        float pr = crealf(c0), pi = cimagf(c0);
        const float r1r = crealf(r1), r1i = cimagf(r1);
        for (int k = 0; k <= kmax; k++) {
            TURN(p + 2 * k, pr, pi);
            STEP(pr, pi, r1r, r1i);
            if (k % 3 == 0 && i < nh) {
                TURN(h + 2 * i, hr, hi);
                STEP(hr, hi, r3r, r3i);
                i++;
            }
        }
    }
    for (; i < nh; i++) {
        TURN(h + 2 * i, hr, hi);
        STEP(hr, hi, r3r, r3i);
    }
#undef TURN
#undef STEP
}

/* Turns the references kept from the previous symbols (channel estimate,
 * previous symbol, its continual pilots) as a timing change of t samples
 * (signal later in the window) turns the carriers. */
static void shift_refs(drx_dvbt* d, double t)
{
    const struct mode* m = &d->m;
    const double w = -2 * M_PI * t / m->n;
    const cf r3 = cexpf(I * (float)(3 * w)), r1 = cexpf(I * (float)w);
    const cf c = cexpf(I * (float)(-m->kc * w));
    turn_chains(d->h3, m->kmax / 3 + 2, d->have_prev ? d->prev : NULL, m->kmax, c, r3, r1);
    if (d->have_prev) {
        for (int i = 0; i < m->ncp; i++)
            d->cp_prev[i] *= cexpf(I * (float)((m->cp[i] - m->kc) * w));
    }
}

static void track_symbol(drx_dvbt* d)
{
    const struct mode* m = &d->m;
    const int n = m->n;
    demod_window(d);
    d->symbols++;
    /* the timing drifts by about the same each symbol: bring the references
     * to this symbol's timing, then only the residual is measured */
    shift_refs(d, d->drift);

    /* scattered pilot phase, from the boosted carriers, until the TPS
     * gives the symbol number */
    if (d->sp < 0) {
        float best = -1;
        for (int p = 0; p < 4; p++) {
            float s = 0;
            for (int k = 3 * p; k <= m->kmax; k += 12)
                s += crealf(d->x[k] * conjf(d->x[k]));
            if (s > best) {
                best = s;
                d->sp = p;
            }
        }
        d->h3_fill = 0;
    }

    /* common phase error against the channel estimate of the continual
     * pilots (updated every symbol), carrier loop */
    if (d->h3_fill) {
        cf s = 0;
        float px = 0, ph = 0;
        for (int i = 0; i < m->ncp; i++) {
            const int k = m->cp[i];
            const cf r = d->h3[k / 3] * m->pref[k];
            s += d->x[k] * conjf(r);
            px += crealf(d->x[k] * conjf(d->x[k]));
            ph += crealf(r * conjf(r));
        }
        const float rho = cabsf(s) / sqrtf(px * ph + 1e-30f);
        d->bad = rho < PILOT_MIN ? d->bad + 1 : 0;
        if (d->bad > LOST_SYMBOLS) {
            restart(d);
            return;
        }
        const float psi = cargf(s);
        const cf derot = cexpf(-I * psi);
        for (int k = 0; k <= m->kmax; k++)
            d->x[k] *= derot;
        d->f += KF * psi / (2 * M_PI * d->nsym_len);
    }

    /* sample clock: phase change of the continual pilots across the band */
    if (d->have_prev) {
        cf lo = 0, hi = 0;
        double klo = 0, khi = 0;
        int nlo = 0, nhi = 0;
        for (int i = 0; i < m->ncp; i++) {
            const int k = m->cp[i];
            const cf c = d->x[k] * conjf(d->cp_prev[i]);
            if (k < m->kc) {
                lo += c;
                klo += k - m->kc;
                nlo++;
            } else {
                hi += c;
                khi += k - m->kc;
                nhi++;
            }
        }
        const double slope = cargf(hi * conjf(lo)) / (khi / nhi - klo / nlo); /* rad per carrier */
        /* a delay of t samples turns carrier k by -2 pi k t / n */
        const double res = -slope * n / (2 * M_PI);
        /* The resampler keeps its nominal ratio (it runs ahead of us, on
         * another thread): the window follows the transmitter's clock by
         * whole samples (tau), and the clock offset is the drift. */
        d->tau += d->drift + res;
        d->drift += (d->tracked < CLOCK_ACQ ? 0.5 : 0.05) * res;
        /* the step that would make the drift vanish; the resampler takes it
         * for the samples to come (the drift prediction follows when they
         * arrive, see set_step) */
        const double target = d->cur_step * (1 + d->drift / d->nsym_len);
        d->clock = target / d->step_nominal - 1;
        if (d->tracked >= 8 && fabs(target / d->step_nominal - 1) < 200e-6)
            d->step = target;
        if (d->tracked >= CLOCK_ACQ)
            d->settled = 1;
    }
    for (int i = 0; i < m->ncp; i++)
        d->cp_prev[i] = d->x[m->cp[i]];

    /* channel estimate: continual pilots every symbol, scattered pilots of
     * this phase */
    for (int i = 0; i < m->ncp; i++) {
        const int k = m->cp[i];
        d->h3[k / 3] = d->x[k] / m->pref[k];
    }
    for (int k = 3 * d->sp; k <= m->kmax; k += 12)
        d->h3[k / 3] = d->x[k] / m->pref[k];
    d->h3_fill |= 1 << d->sp;
    if (d->h3_fill == 15) {
        float s = 0;
        cf ph = 0;
        for (int i = 0; i <= m->kmax / 3; i++) {
            s += crealf(d->h3[i] * conjf(d->h3[i]));
            if (i)
                ph += d->h3[i] * conjf(d->h3[i - 1]);
        }
        d->hmean = s / (float)(m->kmax / 3 + 1);
        const float phi = cargf(ph);
        d->slope_back = cexpf(-I * phi);
        d->slope1 = cexpf(I * phi / 3);
        d->slope2 = cexpf(I * 2 * phi / 3);
    }

    /* TPS: differential bit between this symbol and the previous one */
    if (d->have_prev) {
        float s = 0;
        for (int i = 0; i < m->ntps; i++) {
            const int k = m->tps[i];
            s += crealf(d->x[k] * conjf(d->prev[k]));
        }
        const int bit = s < 0;
        d->tps_sync = (d->tps_sync << 1 | (uint32_t)bit) & 0xFFFF;
        if (d->l >= 0) {
            d->l = (d->l + 1) % FRAME;
            d->tps[d->l] = (uint8_t)bit;
            if (d->l == 16 && d->tps_sync != TPS_SYNC && d->tps_sync != (~TPS_SYNC & 0xFFFF))
                d->l = -1; /* lost the frame */
            else if (d->l == FRAME - 1)
                parse_tps(d);
        } else if (d->tps_sync == TPS_SYNC || d->tps_sync == (~TPS_SYNC & 0xFFFF)) {
            d->l = 16;
            for (int i = 1; i <= 16; i++)
                d->tps[i] = (uint8_t)((d->tps_sync >> (16 - i)) & 1);
        }
        if (d->l >= 0 && (d->l & 3) != d->sp) {
            /* the pilot phase guess and the frame disagree: trust the TPS */
            d->sp = d->l & 3;
            d->h3_fill = 0;
        }
    }
    memcpy(d->prev, d->x, sizeof(cf) * (m->kmax + 1));
    d->have_prev = 1;

    d->tracked++;
    /* lost: no TPS frame for three frames, or no TS packet sync three times
     * in a row (the window may be off the guard interval after a jump) */
    d->no_tps = d->l < 0 ? d->no_tps + 1 : 0;
    if (d->no_tps > 3 * FRAME) {
        restart(d);
        return;
    }
    if (d->params && d->l >= 0 && d->h3_fill == 15 && d->settled) {
        const int ns = demap_symbol(d);
        inner(d, d->soft, ns);
    }
    if (d->back.outer.packets != d->packets_seen) {
        d->packets_seen = d->back.outer.packets;
        d->outer_fails = 0;
    }
    if (d->back.outer.lost && d->streaming) {
        /* the TS sync was lost: find the puncturing phase again, or start
         * over */
        if (++d->outer_fails >= 3) {
            d->outer_fails = 0;
            restart(d);
            return;
        }
        d->streaming = 0;
        d->nsearch = 0;
        d->back.outer.lost = 0;
        d->resyncs++;
    }
    d->sp = (d->sp + 1) & 3;
}

/* Drop the samples before pos (pos may be past the end: the next window
 * starts in samples still to come). */
static void compact(drx_dvbt* d)
{
    const int drop = d->pos < d->nbuf ? d->pos : d->nbuf;
    if (drop <= 0)
        return;
    memmove(d->buf, d->buf + drop, sizeof(cf) * (d->nbuf - drop));
    d->nbuf -= drop;
    d->pos -= drop;
}

int drx_dvbt_resample(drx_dvbt* d, const float* iq, int n, float* out, int max_out,
                      double* step)
{
    *step = d->step;
    resamp_set_ratio(&d->rs, *step);
    return resamp_run(&d->rs, (const cf*)iq, n, (cf*)out, max_out);
}

/* The samples coming were resampled with this step: the timing drift of
 * the window changes with it. */
static void set_step(drx_dvbt* d, double step)
{
    if (step == d->cur_step)
        return;
    if (d->cur_step > 0 && d->state == ST_TRACK) {
        const double truth = d->cur_step * (1 + d->drift / d->nsym_len);
        d->drift = (truth / step - 1) * d->nsym_len;
    }
    d->cur_step = step;
}

/* Demodulates what the buffer holds. */
static void run(drx_dvbt* d)
{
    for (;;) {
        if (d->state == ST_DETECT) {
            if (d->nbuf - d->pos < DETECT_LEN)
                break;
            const int r = detect(d);
            if (r <= 0) {
                d->pos += DETECT_LEN / 2;
                continue;
            }
            d->state = ST_INTCFO;
            d->have_prev = 0;
            continue;
        }
        if (d->nbuf - d->pos < d->m.n + 4)
            break;
        if (d->state == ST_INTCFO) {
            /* INTCFO works on the FFT bins (d->win) of two windows */
            demod_window(d);
            if (d->have_prev) {
                intcfo(d);
                d->state = ST_TRACK;
                d->have_prev = 0;
                d->sp = -1;
                d->l = -1;
                d->tps_sync = 0;
                d->h3_fill = 0;
                d->bad = 0;
                d->tau = 0;
                d->drift = 0;
                d->tracked = 0;
                d->settled = 0;
                d->no_tps = 0;
            } else {
                memcpy(d->prev, d->win, sizeof(cf) * d->m.n);
                d->have_prev = 1;
            }
            advance(d, d->nsym_len);
            continue;
        }
        track_symbol(d);
        if (d->state != ST_TRACK)
            continue;
        /* keep the window where the guard interval protects it */
        int adj = 0;
        if (d->tau > 1.5 || d->tau < -1.5)
            adj = (int)lrint(d->tau);
        if (adj) {
            d->tau -= adj;
            /* the window moves later: the signal sits earlier in it */
            shift_refs(d, -adj);
        }
        advance(d, d->nsym_len + adj);
    }
    /* drop what is behind the next window */
    if (d->pos > d->cap / 2)
        compact(d);
}

/* Room for n more samples in the buffer. */
static int reserve(drx_dvbt* d, int n)
{
    if (d->nbuf + n <= d->cap)
        return 0;
    compact(d);
    while (d->nbuf + n > d->cap) {
        cf* b = realloc(d->buf, sizeof(cf) * d->cap * 2);
        if (!b)
            return -1;
        d->buf = b;
        d->cap *= 2;
    }
    return 0;
}

void drx_dvbt_gap(drx_dvbt* d)
{
    if (d->state != ST_DETECT)
        restart(d);
    d->pos = d->nbuf; /* the samples before the gap are of no use */
}

void drx_dvbt_demod(drx_dvbt* d, const float* x, int n, double step, drx_dvbt_cb cb, void* ctx)
{
    set_step(d, step);
    d->back.outer.cb = cb;
    d->back.outer.ctx = ctx;
    if (reserve(d, n))
        return;
    memcpy(d->buf + d->nbuf, x, sizeof(cf) * n);
    d->nbuf += n;
    run(d);
}

void drx_dvbt_process(drx_dvbt* d, const float* iq, int n, drx_dvbt_cb cb, void* ctx)
{
    d->back.outer.cb = cb;
    d->back.outer.ctx = ctx;
    const int room = (int)(n / (d->step_nominal * 0.99)) + 64;
    if (reserve(d, room))
        return;
    double step;
    d->nbuf += drx_dvbt_resample(d, iq, n, (float*)(d->buf + d->nbuf), d->cap - d->nbuf, &step);
    set_step(d, step);
    run(d);
}

void drx_dvbt_flush(drx_dvbt* d)
{
    flush_block(d);
    while (d->threaded && d->decoded < d->pushed) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}
