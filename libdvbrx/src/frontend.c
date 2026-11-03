/*
 * Receiver front-end: RRC matched filter, Gardner symbol timing recovery with
 * cubic interpolation, and symbol-domain AGC.
 *
 * The matched filter runs at the input rate on fixed-size chunks with plain
 * loops laid out for compiler auto-vectorisation. The timing loop then picks
 * two interpolants per symbol (strobe and mid-point) from the filtered
 * samples, so its per-symbol cost does not depend on the filter length.
 *
 * Equation references: M. Rice, "Digital Communications: A Discrete-Time
 * Approach".
 */
#include "dvbrx/frontend.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 4096 /* input samples filtered per step */
#define NACC 8     /* 8-float vectors accumulated in registers by the filter */
#define NCO_BLK 64 /* samples rotated from one exact phasor */

/* Below UP_SPS samples per symbol, the matched filter output is
 * interpolated by 2 (half-band, HB_P taps on each side of the new samples)
 * before the timing loop: the cubic interpolator needs the signal well
 * below Nyquist, its error costs ~1.7 dB of MER at 1.75 samples per symbol
 * (256APSK at 22 dB) and makes lock impossible below 1.5. */
#ifndef UP_SPS
#define UP_SPS 2.5f
#endif
#define HB_P 16

/* Symbols between the computation of a timing correction and the strobe it
 * first moves, beyond the next one (see timing()). */
#ifndef TDELAY
#define TDELAY 4
#endif

typedef float v8f __attribute__((vector_size(32)));
typedef float v4f __attribute__((vector_size(16)));

/* struct members and buffers are not 32-byte aligned */
static inline v8f ld8(const float* p)
{
    v8f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void st8(float* p, v8f v) { memcpy(p, &v, sizeof(v)); }

struct drx_frontend {
    drx_frontend_cfg cfg;

    int ntaps;
    float* taps; /* unit energy */

    float* x; /* interleaved I/Q: (ntaps - 1) history samples + CHUNK */
    int nhist;

    float* y; /* interleaved I/Q matched filter output awaiting the timing loop */
    int ny, ycap;
    int64_t y0; /* absolute index (in y samples, see up) of the sample y[0] is centred on */

    int up;      /* y samples per input sample: 1, or 2 with the half-band */
    float* hb;   /* half-band input: 2 HB_P history samples + CHUNK */
    float hbc[HB_P];

    /* Sample positions in 32.32 fixed point (samples << 32): the integer
     * part indexes y, the fraction is the interpolation phase. Advancing
     * the strobe is then one integer addition, where a float position
     * needed an add, a floor and a subtraction in the loop's latency chain;
     * it is also exact (the float position drifted by its rounding). */
    int64_t tpos;  /* next strobe position relative to y[0] */
    int64_t thalf; /* sps / 2: mid-point before the strobe */
    int64_t tsps;  /* sps: nominal advance per symbol */
    float tsps_f;  /* sps * 2^32: from symbol periods to the fixed point */
    float prev_re, prev_im;
    float vi; /* loop filter integrator, in fixed-point units (symbol periods * tsps_f) */
    int64_t tadv[TDELAY]; /* corrected advances waiting, oldest first */
    float K1, K2;

    double nco_freq, nco_phase; /* cycles per sample, cycles */
    double tab_freq;            /* frequency nco_re/nco_im were made for */
    float nco_re[2 * NCO_BLK], nco_im[2 * NCO_BLK]; /* exp(-j 2 pi f k), parts duplicated */

    float gain;
    int gain_init;
    double pow_acc;
    int pow_n;
};

static double rrc(double t, double a)
{
    if (fabs(t) < 1e-9)
        return 1.0 - a + 4.0 * a / M_PI;
    if (a > 0 && fabs(fabs(4.0 * a * t) - 1.0) < 1e-9)
        return a / M_SQRT2 *
               ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * a)) +
                (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * a)));
    double num = sin(M_PI * t * (1.0 - a)) + 4.0 * a * t * cos(M_PI * t * (1.0 + a));
    double den = M_PI * t * (1.0 - 16.0 * a * a * t * t);
    return num / den;
}

void drx_frontend_cfg_default(drx_frontend_cfg* cfg, float sps, float rolloff)
{
    cfg->sps = sps;
    cfg->rolloff = rolloff;
    cfg->rrc_delay = 10;
    /* The Gardner detector's self-noise grows quickly as the roll-off
     * shrinks; the loop only has to follow a clock offset of a few ppm, so
     * narrow it accordingly (MER floor about 40 dB down to 0.05). */
    const float r = rolloff / 0.2f;
    cfg->loop_bw = r >= 1.0f ? 1e-3f : 1e-3f * r * r * r;
    cfg->damping = 1.0f;
    cfg->agc_alpha = 0.1f;
}

static void set_loop_gains(drx_frontend* fe)
{
    /* Gardner TED gain (S-curve slope at the origin), same derivation as
     * gr-dvbs2rx: Eq. (8.47) with unit channel gain and unit symbol energy. */
    const double a = fe->cfg.rolloff;
    const double L = 1e3;
    const double C = sin(M_PI * a / 2) / (4 * M_PI * (1 - a * a / 4));
    const double Kp = (8 * C * sin(2 * M_PI / L)) / (2.0 / L);

    /* PI loop updated once per symbol, so Bn*T is the loop_bw as given
     * (Eqs. C.56 and C.60, with a unit NCO gain). */
    const double z = fe->cfg.damping;
    const double th = fe->cfg.loop_bw / (z + 1.0 / (4 * z));
    const double d = 1 + 2 * z * th + th * th;
    fe->K1 = (float)((4 * z * th / d) / Kp);
    fe->K2 = (float)((4 * th * th / d) / Kp);
}

drx_frontend* drx_frontend_new(const drx_frontend_cfg* cfg)
{
    if (cfg->sps < 1.1f || cfg->sps > 64.0f || cfg->rrc_delay < 1)
        return NULL;

    drx_frontend* fe = calloc(1, sizeof(*fe));
    if (!fe)
        return NULL;
    fe->cfg = *cfg;

    int half_len = (int)lround(cfg->rrc_delay * cfg->sps);
    fe->ntaps = 2 * half_len + 1;
    fe->taps = malloc(sizeof(float) * fe->ntaps);
    fe->nhist = fe->ntaps - 1;
    fe->x = calloc(2 * (size_t)(fe->nhist + CHUNK), sizeof(float));
    fe->up = cfg->sps < UP_SPS ? 2 : 1;
    const float ysps = cfg->sps * fe->up; /* samples per symbol in y */
    fe->ycap = fe->up * CHUNK + 2 * (int)ceilf(ysps) + 16;
    fe->y = malloc(sizeof(float) * 2 * fe->ycap);
    if (fe->up > 1)
        fe->hb = calloc(2 * (size_t)(2 * HB_P + CHUNK), sizeof(float));
    if (!fe->taps || !fe->x || !fe->y || (fe->up > 1 && !fe->hb)) {
        drx_frontend_free(fe);
        return NULL;
    }

    double energy = 0;
    for (int i = 0; i < fe->ntaps; i++) {
        double v = rrc((i - half_len) / cfg->sps, cfg->rolloff);
        fe->taps[i] = (float)v;
        energy += v * v;
    }
    for (int i = 0; i < fe->ntaps; i++)
        fe->taps[i] /= (float)sqrt(energy);

    /* Half-band: the new sample half way between x[m] and x[m + 1] is
     * sum c_j (x[m - j] + x[m + 1 + j]), a Kaiser windowed sinc (about 50 dB
     * of image rejection with the band edge at 0.44 of the input rate:
     * 45 Mbaud, roll-off 0.2, at 61.44 Msps). */
    if (fe->up > 1) {
        const double beta = 5.0, span = HB_P + 0.5;
        double sum = 0;
        for (int j = 0; j < HB_P; j++) {
            const double t = j + 0.5, r = t / span;
            /* I0 by its series */
            double a = beta * sqrt(1 - r * r), i0 = 1, term = 1, i0b = 1, tb = 1;
            for (int k = 1; k < 30; k++) {
                term *= (a / (2 * k)) * (a / (2 * k));
                i0 += term;
                tb *= (beta / (2 * k)) * (beta / (2 * k));
                i0b += tb;
            }
            const double c = sin(M_PI * t) / (M_PI * t) * i0 / i0b;
            fe->hbc[j] = (float)c;
            sum += 2 * c;
        }
        for (int j = 0; j < HB_P; j++) /* unit gain at DC */
            fe->hbc[j] /= (float)sum;
    }

    /* The zero history makes y[0] centre on input index -half_len, or
     * HB_P + 1 samples before that through the half-band. */
    fe->y0 = fe->up > 1 ? (int64_t)fe->up * (-half_len - HB_P - 1) : -half_len;
    fe->thalf = llround(ldexp(ysps / 2.0, 32));
    fe->tsps = llround(ldexp(ysps, 32));
    fe->tsps_f = ldexpf(ysps, 32);
    fe->tpos = (int64_t)((int)(ysps / 2) + 2) << 32;
    for (int j = 0; j < (int)(sizeof(fe->tadv) / sizeof(fe->tadv[0])); j++)
        fe->tadv[j] = fe->tsps;
    fe->tab_freq = NAN;
    fe->gain = 1.0f;
    set_loop_gains(fe);
    return fe;
}

void drx_frontend_free(drx_frontend* fe)
{
    if (!fe)
        return;
    free(fe->taps);
    free(fe->x);
    free(fe->y);
    free(fe->hb);
    free(fe);
}

float drx_frontend_gain(const drx_frontend* fe) { return fe->gain; }

void drx_frontend_set_freq(drx_frontend* fe, double freq) { fe->nco_freq = freq; }

/* Copy n interleaved samples from in to x multiplied by exp(-j 2 pi (phase + f k))
 * (one pass over the data).
 * An exact phasor (double) starts each block of NCO_BLK samples, and a table
 * of exp(-j 2 pi f k) over a block gives the phasor of each sample from it:
 * no recurrence, so the vectors of a block are independent (the phasor
 * recurrence used before was a multiply-add latency chain per sample). */
static void nco(drx_frontend* fe, const float* in, float* x, int n)
{
    if (fe->nco_freq == 0) {
        memcpy(x, in, sizeof(float) * 2 * n);
        return;
    }
    const double w = -2 * M_PI * fe->nco_freq;
    if (fe->tab_freq != fe->nco_freq) {
        for (int k = 0; k < NCO_BLK; k++) {
            fe->nco_re[2 * k] = fe->nco_re[2 * k + 1] = (float)cos(w * k);
            fe->nco_im[2 * k] = fe->nco_im[2 * k + 1] = (float)sin(w * k);
        }
        fe->tab_freq = fe->nco_freq;
    }
    const v8f sgn = { -1, 1, -1, 1, -1, 1, -1, 1 };
    double ph = -2 * M_PI * fe->nco_phase;
    int i0 = 0;
    for (; i0 + NCO_BLK <= n; i0 += NCO_BLK) {
        const float c = (float)cos(ph), s = (float)sin(ph);
        /* phasor of sample k: (c + js) T[k], as [re, re, ...] and
         * [-im, im, ...] for the interleaved complex multiply */
        const v8f vc = { c, c, c, c, c, c, c, c }, vs = { s, s, s, s, s, s, s, s };
        const v8f vcs = sgn * vc, vss = sgn * vs;
        const float* ib = in + 2 * i0;
        float* xb = x + 2 * i0;
        for (int k = 0; k < 2 * NCO_BLK; k += 8) {
            const v8f tr = ld8(fe->nco_re + k), ti = ld8(fe->nco_im + k);
            const v8f pr = tr * vc - ti * vs, pi = tr * vss + ti * vcs;
            const v8f a = ld8(ib + k);
            const v8f as = __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6);
            st8(xb + k, a * pr + as * pi);
        }
        ph += w * NCO_BLK;
    }
    if (i0 < n) {
        const float c = (float)cos(ph), s = (float)sin(ph);
        for (int k = 0; k < n - i0; k++) {
            const float tr = fe->nco_re[2 * k], ti = fe->nco_im[2 * k];
            const float pr = tr * c - ti * s, pi = tr * s + ti * c;
            float* xk = x + 2 * (i0 + k);
            const float re = in[2 * (i0 + k)], im = in[2 * (i0 + k) + 1];
            xk[0] = re * pr - im * pi;
            xk[1] = re * pi + im * pr;
        }
    }
    fe->nco_phase += fe->nco_freq * n;
    fe->nco_phase -= floor(fe->nco_phase);
}

float drx_frontend_timing_drift(const drx_frontend* fe) { return fe->vi / fe->tsps_f; }

/* Frequency shift and matched filter of n (<= CHUNK) input samples into y,
 * which may alias in. */
static void mf(drx_frontend* fe, const float* in, int n, float* y)
{
    const int ntaps = fe->ntaps;
    float* restrict x = fe->x;
    nco(fe, in, x + 2 * fe->nhist, n);
    const float* restrict h = fe->taps;

    /* Blocks of NACC vectors stay in registers while looping over the taps:
     * 8 accumulators cover the FMA latency (4 cycles, 2 per cycle), and
     * the tap broadcast is shared by 8 loads. Then single vectors, then
     * the plain path for the last floats. */
    const int m = 2 * n;
    int i0 = 0;
    for (; i0 + 8 * NACC <= m; i0 += 8 * NACC) {
        v8f acc[NACC] = { 0 };
        const float* xs = x + i0;
        for (int j = 0; j < ntaps; j++, xs += 2) {
            const float hj = h[j];
            for (int u = 0; u < NACC; u++)
                acc[u] += hj * ld8(xs + 8 * u);
        }
        for (int u = 0; u < NACC; u++)
            st8(y + i0 + 8 * u, acc[u]);
    }
    for (; i0 + 8 <= m; i0 += 8) {
        v8f acc = { 0 };
        for (int j = 0; j < ntaps; j++)
            acc += h[j] * ld8(x + i0 + 2 * j);
        st8(y + i0, acc);
    }
    for (int i = i0; i < m; i++) {
        float acc = 0;
        for (int j = 0; j < ntaps; j++)
            acc += h[j] * x[i + 2 * j];
        y[i] = acc;
    }
    memmove(x, x + 2 * n, sizeof(float) * 2 * fe->nhist);
}

/* Interpolation by 2 of the n (<= CHUNK) samples in fe->hb after its
 * history into y (2 n samples): x[m] then the half-band sample after it,
 * HB_P + 1 input samples late. */
static void halfband(drx_frontend* fe, int n, float* y)
{
    float* restrict s = fe->hb;
    const float* c = fe->hbc;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        /* 4 complex samples per vector: outputs for m = i + HB_P - 1 .. + 3 */
        const float* xm = s + 2 * (i + HB_P - 1);
        v8f acc0 = { 0 }, acc1 = { 0 };
        for (int j = 0; j < HB_P; j += 2) {
            acc0 += c[j] * (ld8(xm - 2 * j) + ld8(xm + 2 * (j + 1)));
            acc1 += c[j + 1] * (ld8(xm - 2 * (j + 1)) + ld8(xm + 2 * (j + 2)));
        }
        const v8f h = acc0 + acc1, x = ld8(xm);
        st8(y + 4 * i, __builtin_shufflevector(x, h, 0, 1, 8, 9, 2, 3, 10, 11));
        st8(y + 4 * i + 8, __builtin_shufflevector(x, h, 4, 5, 12, 13, 6, 7, 14, 15));
    }
    for (; i < n; i++) {
        const float* xm = s + 2 * (i + HB_P - 1);
        float re = 0, im = 0;
        for (int j = 0; j < HB_P; j++) {
            re += c[j] * (xm[-2 * j] + xm[2 * (j + 1)]);
            im += c[j] * (xm[-2 * j + 1] + xm[2 * (j + 1) + 1]);
        }
        y[4 * i] = xm[0];
        y[4 * i + 1] = xm[1];
        y[4 * i + 2] = re;
        y[4 * i + 3] = im;
    }
    memmove(s, s + 2 * n, sizeof(float) * 2 * 2 * HB_P);
}

/* Make n filtered samples available to the timing loop: filter them from
 * the input (filtered == 0) or copy already filtered ones, then
 * interpolate them by fe->up. */
static void append(drx_frontend* fe, const float* in, int n, int filtered)
{
    float* y = fe->y + 2 * fe->ny;
    if (fe->up > 1) {
        float* hbin = fe->hb + 2 * 2 * HB_P;
        if (filtered)
            memcpy(hbin, in, sizeof(float) * 2 * n);
        else
            mf(fe, in, n, hbin);
        halfband(fe, n, y);
        n *= fe->up;
    } else if (filtered)
        memcpy(y, in, sizeof(float) * 2 * n);
    else
        mf(fe, in, n, y);
    if (!fe->gain_init) {
        double p = 0;
        for (int i = 0; i < 2 * n; i++)
            p += (double)y[i] * y[i];
        if (p > 0) {
            fe->gain = (float)(1.0 / sqrt(p / n));
            fe->gain_init = 1;
        }
    }
    fe->ny += n;
}

/* Cubic Lagrange interpolation between y[i] and y[i + 1] at fraction mu:
 * coefficients c0..c3 of the samples i - 1 .. i + 2 as polynomials in
 * v = 1 + mu (nodes v = 0..3), which comes from the fixed-point fraction
 * with integer operations only (frac1()); rows A..D below:
 * c = (A v + B) v^2 + (C v + D) (Estrin: shorter latency than Horner), each
 * coefficient duplicated for the real and imaginary parts of the
 * interleaved samples. */
static const v8f LA = { -1.f / 6, -1.f / 6, 0.5f, 0.5f, -0.5f, -0.5f, 1.f / 6, 1.f / 6 };
static const v8f LB = { 1, 1, -2.5f, -2.5f, 2, 2, -0.5f, -0.5f };
static const v8f LC = { -11.f / 6, -11.f / 6, 3, 3, -1.5f, -1.5f, 1.f / 3, 1.f / 3 };
static const v8f LD = { 1, 1, 0, 0, 0, 0, 0, 0 };

#define VI_MAX 2e-3f  /* clock offset, fraction of the symbol period */
#define STEP_MAX 0.1f /* timing adjustment per symbol */

/* 1 + the fraction of a fixed-point position: its 23 high bits as the
 * mantissa of a float in [1, 2) (an int-to-float conversion and a scaling
 * took 10 cycles of the loop's latency chain) */
static inline float frac1(int64_t p)
{
    const uint32_t b = 0x3f800000u | (uint32_t)p >> 9;
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

#if TDELAY < 1
#error "TDELAY must be at least 1"
#endif

struct tloop {
    const float* y;
    v8f A, B, C, D; /* interpolator rows, AGC gain included */
    float K1, K2, vi_max, step_max;
    int64_t half, sps;
};

/* One symbol: the two interpolants at pos, the output, the Gardner error and
 * the loop filter; returns the advance it asks for. */
static inline int64_t tsym(const struct tloop* l, int64_t pos, v4f* prev, float* vi, float* out)
{
    const int i = (int)(pos >> 32);
    const int64_t mpos = pos - l->half;
    const int im = (int)(mpos >> 32);
    const float v = frac1(pos), vm = frac1(mpos);

    /* strobe and mid-point interpolants, then the sums of the 4 products
     * per part: t = [s_re, s_im, m_re, m_im] */
    const v8f vs = { v, v, v, v, v, v, v, v }, vs2 = vs * vs;
    const v8f vms = { vm, vm, vm, vm, vm, vm, vm, vm }, vms2 = vms * vms;
    const v8f ps = ((l->A * vs + l->B) * vs2 + (l->C * vs + l->D)) * ld8(l->y + 2 * (i - 1));
    const v8f pm = ((l->A * vms + l->B) * vms2 + (l->C * vms + l->D)) * ld8(l->y + 2 * (im - 1));
    const v4f hs = __builtin_shufflevector(ps, ps, 0, 1, 2, 3) + __builtin_shufflevector(ps, ps, 4, 5, 6, 7);
    const v4f hm = __builtin_shufflevector(pm, pm, 0, 1, 2, 3) + __builtin_shufflevector(pm, pm, 4, 5, 6, 7);
    const v4f t = __builtin_shufflevector(hs, hm, 0, 1, 4, 5) + __builtin_shufflevector(hs, hm, 2, 3, 6, 7);
    memcpy(out, &t, 2 * sizeof(float));

    /* Gardner TED, m . (prev - s): negative when sampling late. */
    const v4f d = (*prev - t) * __builtin_shufflevector(t, t, 2, 3, 2, 3);
    const float e = d[0] + d[1];
    *prev = t;

    /* The integrator follows the clock offset (ppm in practice): bounded,
     * it cannot run away at low SNR and lock on half or twice the symbol
     * rate. The bounds are branches (almost never taken) rather than
     * min/max: off the loop's latency chains. Written so that a NaN ends at
     * the lower bound, as with fmaxf/fminf before. */
    float w = *vi + l->K2 * e;
    if (!(fabsf(w) <= l->vi_max))
        w = w > 0 ? l->vi_max : -l->vi_max;
    *vi = w;
    float st = l->K1 * e + w;
    if (!(fabsf(st) <= l->step_max))
        st = st > 0 ? l->step_max : -l->step_max;
    return l->sps + (int64_t)st;
}

/* The next sampling instant uses the correction computed TDELAY symbols
 * earlier: a little extra loop delay (nothing for a loop this narrow), but
 * the position of the next symbols no longer waits for this symbol's
 * interpolations and error, so successive symbols overlap in the pipeline
 * (the loop was latency bound). The waiting advances are a ring, slot j for
 * the symbols j modulo TDELAY: the loop is unrolled by TDELAY so that each
 * slot stays in a register. */
static int timing(drx_frontend* fe, float* out, int max_out, uint64_t* strobe)
{
    const float g = fe->gain, f = fe->tsps_f;
    /* the loop works in fixed-point units: no scaling of the correction */
    const struct tloop l = { fe->y, g * LA, g * LB, g * LC, g * LD, fe->K1 * f, fe->K2 * f,
                             VI_MAX * f, STEP_MAX * f, fe->thalf, fe->tsps };
    const int64_t end = (int64_t)(fe->ny - 2) << 32; /* strobe < end: i + 2 < ny */
    int64_t pos = fe->tpos;
    float vi = fe->vi;
    int64_t advs[TDELAY];
    memcpy(advs, fe->tadv, sizeof(advs));
    v4f prev = { fe->prev_re, fe->prev_im, 0, 0 };
    int k = 0, j = 0;

    for (;;) {
#pragma GCC unroll 8
        for (j = 0; j < TDELAY; j++) {
            if (pos >= end || k == max_out)
                goto done;
            if (strobe) {
                int64_t a = fe->y0 + ((pos + ((int64_t)1 << 31)) >> 32);
                strobe[k] = a > 0 ? (uint64_t)a / (unsigned)fe->up : 0;
            }
            const int64_t adv = tsym(&l, pos, &prev, &vi, out + 2 * k);
            k++;
            pos += advs[j];
            advs[j] = adv;
        }
    }
done:
    fe->tpos = pos;
    fe->vi = vi;
    for (int m = 0; m < TDELAY; m++) /* oldest first again */
        fe->tadv[m] = advs[(j + m) % TDELAY];
    fe->prev_re = prev[0];
    fe->prev_im = prev[1];

    /* output power for the AGC, apart: in the loop, the accumulator was
     * one more register than there is */
    v8f p8 = { 0 };
    int n = 0;
    for (; n + 8 <= 2 * k; n += 8) {
        const v8f v = ld8(out + n);
        p8 += v * v;
    }
    float p = ((p8[0] + p8[4]) + (p8[1] + p8[5])) + ((p8[2] + p8[6]) + (p8[3] + p8[7]));
    for (; n < 2 * k; n++)
        p += out[n] * out[n];
    fe->pow_acc += p;
    fe->pow_n += k;
    return k;
}

/* Drop filtered samples that the timing loop no longer needs. */
static void compact(drx_frontend* fe)
{
    int drop = (int)((fe->tpos - fe->thalf) >> 32) - 2;
    if (drop <= 0)
        return;
    if (drop > fe->ny)
        drop = fe->ny;
    memmove(fe->y, fe->y + 2 * drop, sizeof(float) * 2 * (fe->ny - drop));
    fe->ny -= drop;
    fe->tpos -= (int64_t)drop << 32;
    fe->y0 += drop;
}

static void update_agc(drx_frontend* fe)
{
    if (fe->pow_n < 256)
        return;
    double p = fe->pow_acc / fe->pow_n;
    /* The output is kept at unit power: far below, the input dropped out
     * (samples lost by the SDR, replaced by zeros), which is not a level
     * change; steps are bounded too (+-6 dB). */
    if (p > 1e-4) {
        double g = pow(1.0 / sqrt(p), fe->cfg.agc_alpha);
        g = g > 2 ? 2 : g < 0.5 ? 0.5 : g;
        fe->gain *= (float)g;
    }
    fe->pow_acc = 0;
    fe->pow_n = 0;
}

static int run(drx_frontend* fe,
               const float* in,
               int n_in,
               int filtered,
               float* out,
               int max_out,
               int* n_consumed,
               uint64_t* strobe)
{
    int consumed = 0, k = 0;
    for (;;) {
        k += timing(fe, out + 2 * k, max_out - k, strobe ? strobe + k : NULL);
        if (k >= max_out || consumed >= n_in)
            break;
        compact(fe);
        update_agc(fe);
        int n = n_in - consumed < CHUNK ? n_in - consumed : CHUNK;
        append(fe, in + 2 * consumed, n, filtered);
        consumed += n;
    }
    *n_consumed = consumed;
    return k;
}

int drx_frontend_process(drx_frontend* fe,
                         const float* in,
                         int n_in,
                         float* out,
                         int max_out,
                         int* n_consumed,
                         uint64_t* strobe)
{
    return run(fe, in, n_in, 0, out, max_out, n_consumed, strobe);
}

void drx_frontend_filter(drx_frontend* fe, const float* in, int n, float* y)
{
    for (int i = 0; i < n; i += CHUNK)
        mf(fe, in + 2 * i, n - i < CHUNK ? n - i : CHUNK, y + 2 * i);
}

int drx_frontend_timing(drx_frontend* fe,
                        const float* y,
                        int n_in,
                        float* out,
                        int max_out,
                        int* n_consumed,
                        uint64_t* strobe)
{
    return run(fe, y, n_in, 1, out, max_out, n_consumed, strobe);
}
