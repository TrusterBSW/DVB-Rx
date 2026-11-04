/*
 * DVB-C (EN 300 429) receiver back half, see dvbc.h.
 *
 * States: ACQ collects symbols for a 4th-power frequency estimate; CMA runs
 * the blind equaliser (constant modulus) with the carrier NCO free-running;
 * then the constellation phase is taken from the 4th power of the
 * equaliser output and DD switches to decision-directed equalisation with a
 * second-order PLL, and decodes. The PLL rotates the equaliser input, so
 * the taps only model the channel.
 *
 * Mapping (EN 300 429 clause 8, 9): the two MSBs of each m-tuple are
 * differentially coded into the quadrant (IkQk); the q = m - 2 LSBs select
 * a point of quadrant 1, rotated into the quadrant by a multiple of pi/2.
 * A pi/2 phase ambiguity thus only rotates the quadrant labels, which the
 * differential decoding ignores.
 */
#include "dvbrx/dvbc.h"
#include "tap.h"
#include "fft.h"
#include "outer.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef float complex cf;

#define ACQ_LEN 16384   /* symbols for the frequency estimate */
#define NTAPS 15        /* T-spaced equaliser */
#define NV 16           /* taps in the vectors: NTAPS, then a zero one */
#define CMA_LEN 40000   /* symbols of blind equalisation */
#define DD_CHECK 20000  /* symbols of DD before the MER is judged */
#define DD_SETTLE 4096  /* symbols of DD before decoding (equaliser, PLL) */
#define MU_CMA 1e-3f
#define MU_DD 2e-4f
#define MER_AVG 1e-3f   /* error power averaging */
#define BITBUF 8192

enum { ST_ACQ, ST_CMA, ST_DD };

/* ---- constellations ------------------------------------------------------ */

struct qamdef {
    int qam, m, grid; /* points, bits per symbol, quadrant 1 grid side */
    float scale;      /* grid units per unit-energy amplitude */
    float r2;         /* CMA modulus, E|a|^4 / E|a|^2 */
    float c4_arg;     /* arg E[a^4] */
    float min_mer;    /* MER below which the lock is judged false, dB */
    uint8_t lut[8][8]; /* quadrant 1 cell (row Q, column I) -> LSBs */
    cf pt[64];         /* quadrant 1 point of each LSB value, unit energy */
    float inv_e[64];   /* 1 / |pt|^2 */
    int square;        /* every cell holds a point: decisions per axis */
    float lvl[8];      /* square: coordinate of cell c, (2c + 1) / scale */
    uint64_t lsb_bytes[64]; /* LSBs of each value as bytes, MSB first (little endian) */
};

/* cross constellations, quadrant 1, (I, Q) of each LSB value (figures 7, 8) */
static const int8_t Q32[8][2] = {
    { 1, 1 }, { 3, 1 }, { 3, 5 }, { 5, 1 }, { 1, 3 }, { 3, 3 }, { 1, 5 }, { 5, 3 },
};
static const uint8_t Q128_ROWS[6][6] = { /* Q = 1, 3, ..., 11; I = 1, 3, ... */
    { 0x00, 0x01, 0x05, 0x04, 0x0C, 0x0D },
    { 0x02, 0x03, 0x07, 0x06, 0x0E, 0x0F },
    { 0x12, 0x13, 0x17, 0x16, 0x1E, 0x1F },
    { 0x10, 0x11, 0x15, 0x14, 0x1C, 0x1D },
    { 0x18, 0x19, 0x09, 0x08, 0xFF, 0xFF },
    { 0x1A, 0x1B, 0x0B, 0x0A, 0xFF, 0xFF },
};

static int gray_to_bin(int g)
{
    int b = 0;
    for (; g; g >>= 1)
        b ^= g;
    return b;
}

static void make_qam(struct qamdef* q, int qam)
{
    memset(q, 0, sizeof(*q));
    q->qam = qam;
    q->m = qam == 16 ? 4 : qam == 32 ? 5 : qam == 64 ? 6 : qam == 128 ? 7 : 8;
    const int nq = 1 << (q->m - 2);
    int coord[64][2];
    if (qam == 32) {
        q->grid = 3;
        for (int v = 0; v < 8; v++) {
            coord[v][0] = Q32[v][0];
            coord[v][1] = Q32[v][1];
        }
    } else if (qam == 128) {
        q->grid = 6;
        for (int r = 0; r < 6; r++)
            for (int c = 0; c < 6; c++)
                if (Q128_ROWS[r][c] != 0xFF) {
                    coord[Q128_ROWS[r][c]][0] = 2 * c + 1;
                    coord[Q128_ROWS[r][c]][1] = 2 * r + 1;
                }
    } else {
        /* square: Q level from b(q-1), b(q-3), ..., I level from b(q-2),
         * ..., b0, both Gray coded */
        const int nb = q->m - 2;
        q->grid = 1 << (nb / 2);
        for (int v = 0; v < nq; v++) {
            int qg = 0, ig = 0;
            for (int k = nb - 1; k >= 0; k--) {
                const int bit = (v >> k) & 1;
                if ((nb - 1 - k) % 2 == 0)
                    qg = qg << 1 | bit;
                else
                    ig = ig << 1 | bit;
            }
            coord[v][0] = 2 * gray_to_bin(ig) + 1;
            coord[v][1] = 2 * gray_to_bin(qg) + 1;
        }
    }
    double e2 = 0, e4 = 0;
    for (int v = 0; v < nq; v++) {
        const double p = coord[v][0] * coord[v][0] + coord[v][1] * coord[v][1];
        e2 += p;
        e4 += p * p;
    }
    e2 /= nq;
    e4 /= nq;
    q->scale = (float)sqrt(e2);
    q->r2 = (float)(e4 / (e2 * e2));
    /* E[a^4] over the 4 rotations of quadrant 1: 4 Re(a^4) / 4 per point */
    double c4 = 0;
    for (int v = 0; v < nq; v++)
        c4 += creal(cpow(coord[v][0] + I * coord[v][1], 4));
    q->c4_arg = c4 >= 0 ? 0.0f : (float)M_PI;
    for (int v = 0; v < nq; v++) {
        q->pt[v] = (coord[v][0] + I * coord[v][1]) / q->scale;
        q->inv_e[v] = 1.0f / crealf(q->pt[v] * conjf(q->pt[v]));
        for (int k = 0; k < q->m - 2; k++)
            q->lsb_bytes[v] |= (uint64_t)((v >> (q->m - 3 - k)) & 1) << (8 * k);
    }
    q->square = qam != 32 && qam != 128;
    for (int c = 0; c < q->grid; c++)
        q->lvl[c] = (float)((2 * c + 1) / (double)q->scale);
    /* nearest point of each grid cell (only cross constellations have empty
     * cells: their corners) */
    for (int r = 0; r < q->grid; r++)
        for (int c = 0; c < q->grid; c++) {
            double best = 1e9;
            for (int v = 0; v < nq; v++) {
                const double di = coord[v][0] - (2 * c + 1), dq = coord[v][1] - (2 * r + 1);
                if (di * di + dq * dq < best) {
                    best = di * di + dq * dq;
                    q->lut[r][c] = (uint8_t)v;
                }
            }
        }
    /* Es/N0 for QEF with RS, about, minus a margin */
    q->min_mer = qam == 16 ? 9 : qam == 32 ? 12 : qam == 64 ? 15 : qam == 128 ? 18 : 21;
}

/* Rotation of quadrant 1 into the quadrant labelled IkQk (table 1: 10 is
 * +pi/2, 11 is +pi, 01 is +3pi/2), and back. */
static const cf ROT[4] = { 1, -I, I, -1 }; /* labels 00, 01, 10, 11 */
static const cf UNROT[4] = { 1, I, -I, -1 };

static inline int quadrant(cf z)
{
    const int left = crealf(z) < 0, down = cimagf(z) < 0;
    /* 00: Q1, 10: Q2 (left, up), 11: Q3, 01: Q4 (right, down) */
    return left ? (down ? 3 : 2) : (down ? 1 : 0);
}

/* Nearest constellation point; *lsb gets its q LSBs, *quad its IkQk. */
static inline cf decide(const struct qamdef* q, cf z, int* quad, int* lsb)
{
    const int k = quadrant(z);
    const cf r = z * UNROT[k] * q->scale;
    int c = (int)((crealf(r) - 1) * 0.5f + 0.5f), w = (int)((cimagf(r) - 1) * 0.5f + 0.5f);
    c = c < 0 ? 0 : c >= q->grid ? q->grid - 1 : c;
    w = w < 0 ? 0 : w >= q->grid ? q->grid - 1 : w;
    const int v = q->lut[w][c];
    *quad = k;
    *lsb = v;
    return q->pt[v] * ROT[k];
}

/* AB of the m-tuple from the previous and current quadrant labels */
static uint8_t DIFF_DEC[4][4];

static void init_diff(void)
{
    for (int prev = 0; prev < 4; prev++)
        for (int ab = 0; ab < 4; ab++) {
            const int a = ab >> 1, b = ab & 1, pi = prev >> 1, pq = prev & 1;
            int i, qq;
            if (!(a ^ b)) {
                i = a ^ pi;
                qq = b ^ pq;
            } else {
                i = a ^ pq;
                qq = b ^ pi;
            }
            DIFF_DEC[prev][i << 1 | qq] = (uint8_t)ab;
        }
}

/* ---- receiver ------------------------------------------------------------ */

struct drx_dvbc {
    struct drx_tap* tap; /* constellation, or NULL */
    enum drx_qam want;
    int try_idx;
    struct qamdef q;
    int state;
    long count;

    cf* acq;
    int nacq;
    struct fft fft;
    cf* spec;

    /* carrier: NCO on the equaliser input */
    double omega, phase; /* rad/symbol, rad */
    float K1, K2;
    cf rot;

    /* equaliser */
    /* taps and delay line, real and imaginary parts apart for 8-float
     * vector arithmetic; tap NTAPS is 0 */
    float wr[NV] __attribute__((aligned(32))), wi[NV] __attribute__((aligned(32)));
    /* the last NV inputs, newest first */
    float xr[NV] __attribute__((aligned(32))), xi[NV] __attribute__((aligned(32)));
    cf ep; /* tap update not applied yet: w += ep conj(x) (see eq_filter) */
    cf y4; /* 4th power accumulator for the phase at the end of CMA */
    float err;  /* decision error power */

    int prev_quad;
    uint8_t* bits;
    int nbits;

    struct outer outer;
    uint64_t packets_at_dd; /* outer packets when DD started */
    int sync_failures;      /* packet sync searches failed since then */
    uint64_t resyncs;
    double shift;

    int inv, inv_mode; /* conjugated input; 0 normal, 1 inverted, 2 auto */
    cf* cbuf;          /* conjugated input block */
};

#define CONJ_BLK 4096

static const int AUTO_ORDER[] = { 64, 256, 128, 32, 16 };

/* Constellation of attempt i: the requested one first (a hint: scan lists
 * are often out of date, e.g. 64-QAM for multiplexes now in 256-QAM),
 * then the others in AUTO_ORDER. */
static int qam_try(const drx_dvbc* d, int i)
{
    if (d->want == DRX_QAM_AUTO)
        return AUTO_ORDER[i];
    if (i == 0)
        return (int)d->want;
    int k = 0;
    for (int j = 0; j < 5; j++)
        if (AUTO_ORDER[j] != (int)d->want && ++k == i)
            return AUTO_ORDER[j];
    return AUTO_ORDER[0];
}

static void start_acq(drx_dvbc* d, int failed)
{
    if (failed) {
        d->resyncs++;
        d->try_idx = (d->try_idx + 1) % 5;
        make_qam(&d->q, qam_try(d, d->try_idx));
        if (d->try_idx == 0 && d->inv_mode == 2) /* every constellation failed */
            d->inv ^= 1;
    }
    d->state = ST_ACQ;
    d->nacq = 0;
}

void drx_dvbc_set_inversion(drx_dvbc* d, int mode)
{
    d->inv_mode = mode == 0 || mode == 1 ? mode : 2;
    d->inv = d->inv_mode == 1;
}

drx_dvbc* drx_dvbc_new(enum drx_qam qam)
{
    if (qam != DRX_QAM_AUTO && qam != DRX_QAM_16 && qam != DRX_QAM_32 && qam != DRX_QAM_64 &&
        qam != DRX_QAM_128 && qam != DRX_QAM_256)
        return NULL;
    init_diff();
    /* aligned: whole-vector stores of the taps and delay line forward to
     * the loads of the next symbol (no cache line split) */
    const size_t size = (sizeof(drx_dvbc) + 63) & ~(size_t)63;
    drx_dvbc* d = aligned_alloc(64, size);
    if (!d)
        return NULL;
    memset(d, 0, size);
    d->want = qam;
    make_qam(&d->q, qam_try(d, 0));
    d->inv_mode = 2;
    d->cbuf = malloc(sizeof(cf) * CONJ_BLK);
    d->acq = malloc(sizeof(cf) * ACQ_LEN);
    d->spec = malloc(sizeof(cf) * ACQ_LEN);
    d->bits = malloc(BITBUF + 8); /* + 8: whole-word stores of each symbol's bits */
    if (!d->cbuf || !d->acq || !d->spec || !d->bits || outer_init(&d->outer) || fft_init(&d->fft, ACQ_LEN)) {
        drx_dvbc_free(d);
        return NULL;
    }
    /* PLL, Bn*T = 1e-3, damping 0.707 */
    const double bnt = 1e-3, z = M_SQRT1_2;
    const double th = bnt / (z + 1 / (4 * z)), dd = 1 + 2 * z * th + th * th;
    d->K1 = (float)(4 * z * th / dd);
    d->K2 = (float)(4 * th * th / dd);
    d->err = 1;
    start_acq(d, 0);
    return d;
}

void drx_dvbc_set_tap(drx_dvbc* d, struct drx_tap* tap) { d->tap = tap; }

void drx_dvbc_free(drx_dvbc* d)
{
    if (!d)
        return;
    outer_free(&d->outer);
    fft_free(&d->fft);
    free(d->spec);
    free(d->acq);
    free(d->bits);
    free(d->cbuf);
    free(d);
}

/* omega is the carrier of the stream as processed, i.e. conjugated when
 * inverted: the upstream shifts and the reported frequency change sign. */
void drx_dvbc_shift(drx_dvbc* d, double delta)
{
    d->omega -= (d->inv ? -2 : 2) * M_PI * delta;
    d->shift += delta;
}

void drx_dvbc_get_stats(const drx_dvbc* d, drx_dvbc_stats* s)
{
    memset(s, 0, sizeof(*s));
    s->locked = d->state == ST_DD && d->outer.synced;
    s->qam = d->state == ST_DD ? d->q.qam : 0;
    s->snr_db = 10.0f * log10f(1.0f / (d->err + 1e-20f));
    s->freq = (float)((d->inv ? -d->omega : d->omega) / (2 * M_PI) + d->shift);
    s->inverted = d->inv;
    s->packets = d->outer.packets;
    s->rs_corrected = d->outer.rs_corrected;
    s->rs_failed = d->outer.rs_failed;
    s->resyncs = d->resyncs;
}

/* Frequency estimate over the acquisition block: the 4th power of a QAM
 * signal holds a spectral line at 4 times the carrier offset (E[a^4] is not
 * zero); its FFT peak, interpolated, gives the offset within +-1/8
 * cycle/symbol. Then the equaliser starts from a pass-through. */
static void start_cma(drx_dvbc* d)
{
    double p = 0;
    for (int k = 0; k < ACQ_LEN; k++) {
        const cf z2 = d->acq[k] * d->acq[k];
        d->spec[k] = z2 * z2;
        p += crealf(d->acq[k] * conjf(d->acq[k]));
    }
    fft_run(&d->fft, d->spec, 0);
    int kmax = 0;
    float best = -1;
    for (int k = 0; k < ACQ_LEN; k++) {
        const float m = crealf(d->spec[k] * conjf(d->spec[k]));
        if (m > best) {
            best = m;
            kmax = k;
        }
    }
    const float a = cabsf(d->spec[(kmax + ACQ_LEN - 1) % ACQ_LEN]), b = cabsf(d->spec[kmax]),
                c = cabsf(d->spec[(kmax + 1) % ACQ_LEN]);
    const float den = a - 2 * b + c;
    const double frac = den != 0 ? 0.5 * (a - c) / den : 0;
    double f4 = (kmax + frac) / ACQ_LEN;
    if (f4 >= 0.5)
        f4 -= 1;
    d->omega = 2 * M_PI * f4 / 4;
    d->phase = 0;
    d->rot = 1;
    memset(d->wr, 0, sizeof(d->wr));
    memset(d->wi, 0, sizeof(d->wi));
    memset(d->xr, 0, sizeof(d->xr));
    memset(d->xi, 0, sizeof(d->xi));
    d->ep = 0;
    d->wr[NTAPS / 2] = (float)(1.0 / sqrt(p / ACQ_LEN + 1e-20));
    d->y4 = 0;
    d->err = 1;
    d->state = ST_CMA;
    d->count = 0;
}

static void start_dd(drx_dvbc* d)
{
    /* the pending tap update first */
    for (int k = 0; k < NTAPS; k++) {
        const cf w = d->wr[k] + I * d->wi[k] + d->ep * conjf(d->xr[k] + I * d->xi[k]);
        d->wr[k] = crealf(w);
        d->wi[k] = cimagf(w);
    }
    d->ep = 0;
    /* rotate the taps so that the constellation sits on its grid (up to a
     * multiple of pi/2) */
    const float th = cargf(d->y4) / 4;
    const cf r = cexpf(-I * th);
    for (int k = 0; k < NTAPS; k++) {
        const cf w = (d->wr[k] + I * d->wi[k]) * r;
        d->wr[k] = crealf(w);
        d->wi[k] = cimagf(w);
    }
    d->state = ST_DD;
    d->count = 0;
    d->err = 1;
    d->prev_quad = 0;
    d->nbits = 0;
    d->packets_at_dd = d->outer.packets;
    d->sync_failures = 0;
    outer_reset(&d->outer);
}

static void flush_bits(drx_dvbc* d)
{
    outer_bits(&d->outer, d->bits, d->nbits);
    d->nbits = 0;
}

/* e^(ja) for |a| <= pi: Taylor polynomials on a quarter turn (error ~1e-9),
 * several times cheaper than cexpf */
static inline cf expj(float a)
{
    const float q = rintf(a * (float)M_2_PI);
    const float r = a - q * (float)M_PI_2, r2 = r * r;
    const float sn = r * (1 + r2 * (-1.0f / 6 + r2 * (1.0f / 120 + r2 * (-1.0f / 5040 + r2 * (1.0f / 362880)))));
    const float cs = 1 + r2 * (-0.5f + r2 * (1.0f / 24 + r2 * (-1.0f / 720 + r2 * (1.0f / 40320))));
    switch ((int)q & 3) {
    case 0: return cs + I * sn;
    case 1: return -sn + I * cs;
    case 2: return -cs - I * sn;
    default: return sn - I * cs;
    }
}

typedef float v8f __attribute__((vector_size(32)));
typedef float v4f __attribute__((vector_size(16)));
typedef int32_t v8i __attribute__((vector_size(32)));

static inline v8f ld8(const float* p)
{
    v8f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void st8(float* p, v8f v) { memcpy(p, &v, sizeof(v)); }

/* [sum a, sum b, sum c, sum d] */
static inline v4f hsum4(v8f a, v8f b, v8f c, v8f d)
{
    const v8f ab = __builtin_shufflevector(a, b, 0, 1, 2, 3, 8, 9, 10, 11) +
                   __builtin_shufflevector(a, b, 4, 5, 6, 7, 12, 13, 14, 15);
    const v8f cd = __builtin_shufflevector(c, d, 0, 1, 2, 3, 8, 9, 10, 11) +
                   __builtin_shufflevector(c, d, 4, 5, 6, 7, 12, 13, 14, 15);
    /* [a0+a2, c0+c2, a1+a3, c1+c3 | b.., d..] */
    const v8f t = __builtin_shufflevector(ab, cd, 0, 8, 1, 9, 4, 12, 5, 13) +
                  __builtin_shufflevector(ab, cd, 2, 10, 3, 11, 6, 14, 7, 15);
    const v4f lo = __builtin_shufflevector(t, t, 0, 1, 2, 3), hi = __builtin_shufflevector(t, t, 4, 5, 6, 7);
    /* [a, b, c, d] */
    return __builtin_shufflevector(lo, hi, 0, 4, 1, 5) + __builtin_shufflevector(lo, hi, 2, 6, 3, 7);
}

/* Scalar state of the loops, in local variables over a run of symbols: kept
 * in the receiver structure, each store to the bit buffer (a char pointer,
 * which may alias anything) sent it back to memory and in again, on the
 * loops' latency chains. The taps and the delay line stay there (too many
 * vector registers; their loads are not in the chains). */
struct eq {
    cf ep; /* pending tap update */
    double phase, omega;
    cf rot;
    float err;
    long count;
};

static inline void eq_load(const drx_dvbc* d, struct eq* q)
{
    q->ep = d->ep;
    q->phase = d->phase;
    q->omega = d->omega;
    q->rot = d->rot;
    q->err = d->err;
    q->count = d->count;
}

static inline void eq_store(drx_dvbc* d, const struct eq* q)
{
    d->ep = q->ep;
    d->phase = q->phase;
    d->omega = q->omega;
    d->rot = q->rot;
    d->err = q->err;
    d->count = q->count;
}

/* NCO: the input rotated by the current phasor, then the phasor of the
 * next symbol. */
static inline cf eq_nco(struct eq* q, cf s)
{
    const cf x = s * q->rot;
    q->phase += q->omega;
    if (q->phase > M_PI)
        q->phase -= 2 * M_PI;
    else if (q->phase < -M_PI)
        q->phase += 2 * M_PI;
    q->rot = expj(-(float)q->phase);
    return x;
}

/* Equaliser output for the new (rotated) input x0, the previous symbol's
 * tap update (LMS or CMA) still pending:
 *   y = sum_k (w_k + ep conj(x_k)) x'_k,   x' = [x0, x_0 .. x_14]
 *     = T + ep U,  T = sum_k w_k x'_k,  U = sum_k conj(x_k) x'_k (k < NTAPS).
 * The sums over the old entries wait neither for ep nor for x0, so the
 * decision-directed loop goes through one complex multiply-add per symbol
 * instead of the tap update, a store and reload of the taps, the products
 * and a horizontal sum (it was latency bound). Then the taps take the
 * update (lane NTAPS stays 0) and x0 enters the line. */
static inline cf eq_filter(drx_dvbc* d, struct eq* q, cf x0)
{
    const v8f z = { 0 };
    const v8i m15 = { -1, -1, -1, -1, -1, -1, -1, 0 };
    const v8f xr0 = ld8(d->xr), xi0 = ld8(d->xi), xr1h = ld8(d->xr + 8), xi1h = ld8(d->xi + 8);
    const v8f wr0 = ld8(d->wr), wi0 = ld8(d->wi), wr1 = ld8(d->wr + 8), wi1 = ld8(d->wi + 8);
    /* old line shifted by one, lane 0 empty; old line without lane NTAPS */
    const v8f sr0 = __builtin_shufflevector(z, xr0, 0, 8, 9, 10, 11, 12, 13, 14);
    const v8f si0 = __builtin_shufflevector(z, xi0, 0, 8, 9, 10, 11, 12, 13, 14);
    const v8f sr1 = __builtin_shufflevector(xr0, xr1h, 7, 8, 9, 10, 11, 12, 13, 14);
    const v8f si1 = __builtin_shufflevector(xi0, xi1h, 7, 8, 9, 10, 11, 12, 13, 14);
    const v8f xr1 = (v8f)((v8i)xr1h & m15), xi1 = (v8f)((v8i)xi1h & m15);

    const v8f tr = (wr0 * sr0 - wi0 * si0) + (wr1 * sr1 - wi1 * si1);
    const v8f ti = (wr0 * si0 + wi0 * sr0) + (wr1 * si1 + wi1 * sr1);
    const v8f ur = (xr0 * sr0 + xi0 * si0) + (xr1 * sr1 + xi1 * si1);
    const v8f ui = (xr0 * si0 - xi0 * sr0) + (xr1 * si1 - xi1 * sr1);
    const v4f h = hsum4(tr, ti, ur, ui);
    /* the x0 terms: w_0 x0 and conj(x_0) x0 */
    const cf w0 = wr0[0] + I * wi0[0], c0 = xr0[0] - I * xi0[0];
    const cf T = h[0] + I * h[1] + w0 * x0, U = h[2] + I * h[3] + c0 * x0;
    const cf ep = q->ep;
    const cf y = T + ep * U;

    const float er = crealf(ep), ei = cimagf(ep);
    st8(d->wr, wr0 + (er * xr0 + ei * xi0));
    st8(d->wi, wi0 + (ei * xr0 - er * xi0));
    st8(d->wr + 8, wr1 + (er * xr1 + ei * xi1));
    st8(d->wi + 8, wi1 + (ei * xr1 - er * xi1));
    const v8f nr = { crealf(x0), 0, 0, 0, 0, 0, 0, 0 }, ni = { cimagf(x0), 0, 0, 0, 0, 0, 0, 0 };
    st8(d->xr, sr0 + nr);
    st8(d->xi, si0 + ni);
    st8(d->xr + 8, sr1);
    st8(d->xi + 8, si1);
    return y;
}

/* Blind equalisation of n symbols at most, until the end of CMA_LEN;
 * returns the number taken. */
static int run_cma(drx_dvbc* d, const cf* s, int n)
{
    struct eq q;
    eq_load(d, &q);
    const float r2 = d->q.r2, K1 = d->K1, K2 = d->K2;
    const int flip = d->q.c4_arg != 0;
    cf y4acc = d->y4;
    int i = 0;
    for (; i < n && q.count < CMA_LEN; i++) {
        const cf y = eq_filter(d, &q, eq_nco(&q, s[i]));
        q.count++;
        q.ep = MU_CMA * y * (r2 - crealf(y * conjf(y)));
        if (q.count > CMA_LEN / 2) {
            /* blind 4th-power PLL: the constellation phase modulo pi/2 */
            const cf y2 = y * y;
            cf y4 = y2 * y2;
            if (flip)
                y4 = -y4;
            const float pe = cimagf(y4) / (cabsf(y4) + 1e-12f) * 0.25f;
            q.phase += 0.5f * K1 * pe;
            q.omega += 0.25f * K2 * pe;
            if (q.count > CMA_LEN - 4096)
                y4acc += y4;
        }
    }
    d->y4 = y4acc;
    eq_store(d, &q);
    return i;
}

/* Decision-directed equalisation, PLL and bits of n symbols at most;
 * stops at symbol DD_CHECK when the MER is too low. Returns the number of
 * symbols taken (not counting that one, which goes to the acquisition). */
static int run_dd(drx_dvbc* d, const cf* s, int n)
{
    struct eq q;
    eq_load(d, &q);
    const struct qamdef* qd = &d->q;
    const float K1 = d->K1, K2 = d->K2, hs = qd->scale * 0.5f;
    const int g1 = qd->grid - 1, m = qd->m, square = qd->square;
    int prev_quad = d->prev_quad, nbits = d->nbits;
    uint8_t* const bits = d->bits;
    int i = 0;
    /* constellation: every 8th equalised symbol while someone looks */
    cf tb[64];
    int nt = 0;
    const int tap = drx_tap_on(d->tap);
    for (; i < n; i++) {
        const cf y = eq_filter(d, &q, eq_nco(&q, s[i]));
        q.count++;
        if (tap && nt < 64 && !(q.count & 7))
            tb[nt++] = y;

        int quad, lsb;
        cf dec;
        if (square) {
            /* the nearest point, axis by axis (decide() for the square
             * constellations, without its rotations and table lookups in
             * the loop's latency chain) */
            const float yr = crealf(y), yi = cimagf(y);
            int cr = (int)(fabsf(yr) * hs), ci = (int)(fabsf(yi) * hs);
            cr = cr < 0 ? 0 : cr > g1 ? g1 : cr;
            ci = ci < 0 ? 0 : ci > g1 ? g1 : ci;
            const float dr = qd->lvl[cr], di = qd->lvl[ci];
            dec = (yr < 0 ? -dr : dr) + I * (yi < 0 ? -di : di);
            quad = quadrant(y);
            /* quadrants 01 and 10 swap the axes in the quadrant 1 frame */
            lsb = quad == 1 || quad == 2 ? qd->lut[cr][ci] : qd->lut[ci][cr];
        } else {
            dec = decide(qd, y, &quad, &lsb);
        }
        const cf e = dec - y;
        q.err += MER_AVG * (crealf(e * conjf(e)) - q.err);
        /* LMS on the taps (next symbol), PLL on the input rotation */
        q.ep = MU_DD * e;
        const float pe = cimagf(y * conjf(dec)) * qd->inv_e[lsb];
        q.phase += K1 * pe;
        q.omega += K2 * pe;

        /* bits: A B from the quadrant change, then the LSBs, MSB first,
         * one 8-byte store */
        const int ab = DIFF_DEC[prev_quad][quad];
        prev_quad = quad;
        if (q.count > DD_SETTLE) {
            const uint64_t w = (uint64_t)(ab >> 1) | (uint64_t)(ab & 1) << 8 | qd->lsb_bytes[lsb] << 16;
            memcpy(bits + nbits, &w, sizeof(w));
            nbits += m;
            if (nbits > BITBUF - 8) {
                d->nbits = nbits;
                flush_bits(d);
                nbits = 0;
            }
        }
        /* written so that a NaN (diverged equalizer) restarts too */
        if (q.count == DD_CHECK && !(10 * log10f(1 / (q.err + 1e-20f)) >= qd->min_mer))
            break;
    }
    d->prev_quad = prev_quad;
    d->nbits = nbits;
    eq_store(d, &q);
    if (nt)
        drx_tap_put(d->tap, (const float*)tb, nt, 1);
    return i;
}

/* Returns the symbols consumed: all of them, or fewer when the inversion
 * hypothesis changed (the rest must be taken the other way). */
static int process(drx_dvbc* d, const cf* s, int n)
{
    const int inv = d->inv;
    int i = 0;
    while (i < n && d->inv == inv) {
        if (d->state == ST_ACQ) {
            while (i < n && d->nacq < ACQ_LEN)
                d->acq[d->nacq++] = s[i++];
            if (d->nacq < ACQ_LEN)
                return i;
            start_cma(d);
            run_cma(d, d->acq, ACQ_LEN);
            continue;
        }
        if (d->state == ST_CMA) {
            i += run_cma(d, s + i, n - i);
            if (d->count >= CMA_LEN)
                start_dd(d);
            continue;
        }
        i += run_dd(d, s + i, n - i);
        flush_bits(d);
        if (d->count >= DD_CHECK && !(10 * log10f(1 / (d->err + 1e-20f)) >= d->q.min_mer)) {
            start_acq(d, 1);
            continue;
        }
        if (d->outer.lost) {
            /* With a plausible MER, the loops are converging (or the
             * stream was cut): keep them and look for the packet sync
             * again; otherwise, or when this constellation never gave
             * packets (a wrong guess can show a fair MER), start over. */
            const int never = d->outer.packets == d->packets_at_dd;
            if (10 * log10f(1 / (d->err + 1e-20f)) >= d->q.min_mer &&
                !(never && ++d->sync_failures >= 3)) {
                d->resyncs++;
                outer_reset(&d->outer);
            } else {
                start_acq(d, 1);
            }
        }
    }
    return i;
}

void drx_dvbc_process(drx_dvbc* d, const float* sym, int n, drx_dvbc_cb cb, void* ctx)
{
    d->outer.cb = cb;
    d->outer.ctx = ctx;
    const cf* s = (const cf*)sym;
    while (n > 0) {
        if (!d->inv) {
            const int k = process(d, s, n);
            s += k;
            n -= k;
            continue;
        }
        const int m = n < CONJ_BLK ? n : CONJ_BLK;
        for (int k = 0; k < m; k++)
            d->cbuf[k] = conjf(s[k]);
        const int k = process(d, d->cbuf, m);
        s += k;
        n -= k;
    }
}
