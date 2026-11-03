/*
 * DVB-S2 physical layer synchronisation, see pl.h.
 *
 * Search: a differential correlation over the SOF and over the PLSC symbol
 * pairs (whose differential pattern is known up to one common sign) finds
 * PLHEADER candidates regardless of the carrier offset. A candidate is
 * accepted when the next header shows up where the decoded PLS says it
 * should.
 *
 * Carrier: the differential correlation gives a coarse frequency, refined
 * on the 90 known header symbols with the Luise & Reggiannini estimator. A
 * second-order PLL then runs over every frame, anchored on the header
 * phase, data-aided on the header and pilots and decision-directed on the
 * data symbols. Amplitude and noise come from the known symbols.
 *
 * Reed-Muller PLSC code and PL scrambling sequence as in gr-dvbs2rx.
 */
#include "dvbrx/pl.h"
#include "dvbrx/demap.h"

#include <complex.h>
#include <math.h>
#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float complex cf;

#define SOF_LEN 26
#define PLSC_LEN 64
#define HDR_LEN 90
#define SLOT_LEN 90
#define PILOT_LEN 36
#define MAX_PILOTS 22
#define MAX_PAYLOAD (360 * SLOT_LEN + MAX_PILOTS * PILOT_LEN)
#define MAX_FRAME (HDR_LEN + MAX_PAYLOAD)
#define CAP (4 * MAX_FRAME)
#define SLIP 2 /* symbols of timing slip tolerated between headers */

/* Equaliser: symbol-spaced complex FIR of EQ_N taps, trained by least
 * squares on the known symbols (header and pilots) of one pilot frame in
 * EQ_EVERY, with forgetting. The analog filters of wideband SDRs (two
 * AD9361 back to back at 45 Mbaud) leave inter-symbol interference that
 * capped the SNR at 18 dB. */
#define EQ_D 7
#define EQ_N (2 * EQ_D + 1)
#define EQ_EVERY 4
#define EQ_EVERY_SLOW 16
#define EQ_LAMBDA 0.7
#define EQ_KEEP 16 /* symbols kept before the frame (>= EQ_D) */

#define TH_DETECT 0.4f  /* normalised header metric, search */
#define TH_CONFIRM 0.3f /* next header, before locking */
#define TH_TRACK 0.2f   /* header while locked */
#define MAX_MISSES 3
#define LR_LAGS 32 /* Luise & Reggiannini estimator */
#ifndef PLL_BLOCK
#define PLL_BLOCK 8 /* symbols per decision-directed loop update, see frame_pll() */
#endif

/* Es/N0 (dB) for quasi-error-free decoding, normal frames, EN 302 307-1
 * Table 13 (AWGN, ideal synchronisation). */
static const float MC_QEF_DB[29] = {
    0,     -2.35f, -1.24f, -0.30f, 1.00f,  2.23f,  3.10f,  4.03f,  4.68f,  5.18f,
    6.20f, 6.42f,  5.50f,  6.62f,  7.91f,  9.35f,  10.69f, 10.98f, 8.97f,  10.21f,
    11.03f, 11.61f, 12.89f, 13.13f, 12.73f, 13.64f, 14.28f, 15.69f, 16.05f,
};

/* Es/N0 needed for QEF decoding: Table 13 for DVB-S2; for DVB-S2X, the
 * Shannon bound of the MODCOD's spectral efficiency plus 1 dB. Only used to
 * spot frames that cannot be decoded (wrong lock), 4 dB below. */
static float qef_db(const drx_modcod* m)
{
    if (m->pls < 0x80)
        return MC_QEF_DB[m->pls >> 2];
    int num = 1, den = 1;
    sscanf(m->rate, "%d_%d", &num, &den);
    const double eta = m->bits * (double)num / den;
    return (float)(10 * log10(pow(2, eta) - 1) + 1);
}

struct pls {
    int plsc, short_frame, pilots, dummy;
    drx_modcod mc;
    int slots, npilots, len; /* len: whole PLFRAME, header included */
};

#define HARD_GRID 128 /* cells per axis of the hard decision table */

struct constellation {
    int n;
    cf p[256];
    float inv_e[256]; /* 1 / |p|^2 */
    drx_demap* dm;    /* fast nearest-point search for dense constellations */
    /* Hard decisions: nearest point for each cell of a grid over the I/Q
     * plane when the whole cell is in one decision region, -1 otherwise
     * (and outside the grid): a table lookup instead of a search on the
     * decision-directed loop's critical path. Up to 128 points. */
    float grid_lo, grid_inv;
    int8_t grid[HARD_GRID * HARD_GRID];
};

struct drx_pl {
    cf* buf;
    int len, pos;
    int locked, misses;
    int frames_locked; /* frames since the last lock */
    int shift_pos;      /* buffer index from which shift_delta applies, -1: none */
    double shift_delta; /* rad/symbol */

    cf hdr_sof[SOF_LEN];
    cf hdr_plsc[256][PLSC_LEN];
    cf dref_sof[SOF_LEN - 1];  /* h[k] conj(h[k-1]) */
    cf dref_plsc[PLSC_LEN / 2]; /* pair differentials, code 0 */
    float plsc_scr[PLSC_LEN]; /* PLSC scrambling, +-1 */
    cf* descr; /* PL descrambling sequence (MAX_PAYLOAD) */

    double omega; /* carrier frequency, rad/symbol */
    float K1, K2;
    float w_phase[PLL_BLOCK], w_freq[PLL_BLOCK]; /* block loop filter, see frame_pll() */
    float amp, n0;
    int have_amp;

    struct constellation* cons[128]; /* by PLS code >> 1 */
    cf* out;
    cf* z; /* derotated frame */
    drx_pl_out_cb get_out;
    uint64_t index;
    drx_pl_stats stats;

    /* equaliser, see EQ_N */
    int eq_on, eq_count, eq_phase;
    cf eq_w[EQ_N];
    double complex eq_R[EQ_N][EQ_N], eq_p[EQ_N];
    cf* yeq;
    /* carrier of the last pilot-aided frame: phase ph[s] + slope[s] k
     * (plus omega0 k) in segment s, see frame_pilot_aided() */
    double fr_ph[MAX_PILOTS + 2], fr_slope[MAX_PILOTS + 2], fr_omega0;
    int fr_nseg;
    float fr_amp;
};

static void rotate(const cf* y, int n, double ph0, double dph, float g, const cf* descr, cf* out);

/* ---- PLHEADER --------------------------------------------------------- */

static inline cf pi2_base(int k) /* pi/2-BPSK point of bit 0 at index k */
{
    return (k & 1) ? (-1.0f + 1.0f * I) * (float)M_SQRT1_2 : (1.0f + 1.0f * I) * (float)M_SQRT1_2;
}

static uint64_t bit_interleave(uint32_t a, uint32_t b)
{
    uint64_t r = 0;
    for (int i = 0; i < 32; i++) {
        r |= (uint64_t)((a >> i) & 1) << (2 * i + 1);
        r |= (uint64_t)((b >> i) & 1) << (2 * i);
    }
    return r;
}

/* Reed-Muller (64, 8) PLS codeword: EN 302 307-1 Section 5.5.2.4 for the
 * DVB-S2 codes (< 128, first row unused), extended by EN 302 307-2. */
#define PLSC_G0 0x90ac2dddu
#define PLSC_SCRAMBLER 0x719d83c953422dfaULL

static uint64_t plsc_encode(int w)
{
    static const uint32_t G[7] = { PLSC_G0, 0x55555555, 0x33333333, 0x0f0f0f0f,
                                   0x00ff00ff, 0x0000ffff, 0xffffffff };
    uint32_t c = 0;
    for (int row = 0; row < 7; row++)
        if (w & (0x80 >> row))
            c ^= G[row];
    return bit_interleave(c, (w & 1) ? ~c : c);
}

static int parity18(long a, long b)
{
    return __builtin_parityl(a & b & 0x3FFFF);
}

static void init_descrambler(drx_pl* pl, int gold_code)
{
    static const cf lut[4] = { 1.0f, -1.0f * I, -1.0f, 1.0f * I };
    long x = 0x00001, y = 0x3FFFF;
    for (int n = 0; n < gold_code; n++) {
        int xb = parity18(x, 0x0081);
        x >>= 1;
        if (xb)
            x |= 0x20000;
    }
    for (int i = 0; i < MAX_PAYLOAD; i++) {
        int xa = parity18(x, 0x8050), xb = parity18(x, 0x0081), xc = x & 1;
        x >>= 1;
        if (xb)
            x |= 0x20000;
        int ya = parity18(y, 0x04A1), yb = parity18(y, 0xFF60), yc = y & 1;
        y >>= 1;
        if (ya)
            y |= 0x20000;
        pl->descr[i] = lut[((xa ^ yb) << 1) | (xc ^ yc)];
    }
}

int drx_pls_root_to_gold(uint32_t root)
{
    /* the x sequence of the descrambler, stepped from its initial state */
    uint32_t x = 1;
    for (int g = 0; g < 0x3FFFF; g++) {
        if (x == root)
            return g;
        x = (((x ^ (x >> 7)) & 1) << 17) | (x >> 1);
    }
    return -1;
}

int drx_pls_parse(const char* s)
{
    char* end;
    const int root = s[0] == 'r' || s[0] == 'R';
    const long v = strtol(s + root, &end, 0);
    if (*end || v < 0 || v > 0x3FFFF)
        return -1;
    return root ? drx_pls_root_to_gold((uint32_t)v) : (int)v;
}

static int parse_pls(int plsc, struct pls* h)
{
    h->plsc = plsc;
    h->pilots = plsc & 1;
    h->dummy = plsc < 4; /* DVB-S2 MODCOD 0 */
    if (h->dummy) {
        h->pilots = 0;
        h->short_frame = 0;
        h->slots = 36;
    } else {
        if (drx_pls_lookup(plsc, &h->mc))
            return -1; /* reserved, or a MODCOD not supported (128/256APSK) */
        h->short_frame = h->mc.short_frame;
        /* 128APSK: 9258 symbols rounded up to 103 slots (dummy symbols) */
        const int nsym = ((h->short_frame ? 16200 : 64800) + h->mc.bits - 1) / h->mc.bits;
        h->slots = (nsym + SLOT_LEN - 1) / SLOT_LEN;
    }
    h->npilots = h->pilots ? (h->slots - 1) / 16 : 0;
    h->len = HDR_LEN + h->slots * SLOT_LEN + h->npilots * PILOT_LEN;
    return 0;
}

/* Normalised differential header correlation at y; *c receives the
 * combined correlation, whose phase is 2 pi f per symbol. */
static float hdr_metric(const drx_pl* pl, const cf* y, cf* c)
{
    cf c1 = 0, c2 = 0;
    float norm = 0;
    for (int k = 1; k < SOF_LEN; k++) {
        c1 += y[k] * conjf(y[k - 1]) * conjf(pl->dref_sof[k - 1]);
        norm += 0.5f * (crealf(y[k] * conjf(y[k])) + crealf(y[k - 1] * conjf(y[k - 1])));
    }
    for (int i = 0; i < PLSC_LEN / 2; i++) {
        const cf* a = y + SOF_LEN + 2 * i;
        c2 += a[1] * conjf(a[0]) * conjf(pl->dref_plsc[i]);
        norm += 0.5f * (crealf(a[0] * conjf(a[0])) + crealf(a[1] * conjf(a[1])));
    }
    *c = c1 + (crealf(c2 * conjf(c1)) >= 0 ? c2 : -c2);
    return (cabsf(c1) + cabsf(c2)) / (norm + 1e-20f);
}

/*
 * First header position p in [from, to) with hdr_metric() > th, or to.
 * The search runs at every symbol, so it works a block of positions at a
 * time on split real / imaginary arrays (taps outer, positions inner, which
 * vectorises): differential products are computed once per symbol and the
 * normalisation comes from prefix sums of the energy.
 */
#define SCAN_BLOCK 1024
static int scan_hdr(const drx_pl* pl, int from, int to, float th)
{
    enum { NX = SCAN_BLOCK + HDR_LEN };
    float dr[NX], di[NX], e[NX + 1];
    float c1r[SCAN_BLOCK], c1i[SCAN_BLOCK], c2r[SCAN_BLOCK], c2i[SCAN_BLOCK];
    for (int p0 = from; p0 < to; p0 += SCAN_BLOCK) {
        const int nb = to - p0 < SCAN_BLOCK ? to - p0 : SCAN_BLOCK;
        const int nx = nb + HDR_LEN - 1; /* symbols p0 .. p0 + nx - 1 */
        const float* y = (const float*)(pl->buf + p0);
        /* d[x] = y[x] conj(y[x - 1]), e: prefix sums of |y|^2 */
        e[0] = 0;
        for (int x = 0; x < nx; x++)
            e[x + 1] = e[x] + y[2 * x] * y[2 * x] + y[2 * x + 1] * y[2 * x + 1];
        dr[0] = di[0] = 0;
        for (int x = 1; x < nx; x++) {
            dr[x] = y[2 * x] * y[2 * x - 2] + y[2 * x + 1] * y[2 * x - 1];
            di[x] = y[2 * x + 1] * y[2 * x - 2] - y[2 * x] * y[2 * x - 1];
        }
        for (int j = 0; j < nb; j++)
            c1r[j] = c1i[j] = c2r[j] = c2i[j] = 0;
        for (int t = 0; t < SOF_LEN - 1; t++) {
            const float wr = crealf(pl->dref_sof[t]), wi = -cimagf(pl->dref_sof[t]);
            const float* a = dr + 1 + t;
            const float* b = di + 1 + t;
            for (int j = 0; j < nb; j++) {
                c1r[j] += a[j] * wr - b[j] * wi;
                c1i[j] += a[j] * wi + b[j] * wr;
            }
        }
        for (int t = 0; t < PLSC_LEN / 2; t++) {
            const float wr = crealf(pl->dref_plsc[t]), wi = -cimagf(pl->dref_plsc[t]);
            const float* a = dr + SOF_LEN + 2 * t + 1;
            const float* b = di + SOF_LEN + 2 * t + 1;
            for (int j = 0; j < nb; j++) {
                c2r[j] += a[j] * wr - b[j] * wi;
                c2i[j] += a[j] * wi + b[j] * wr;
            }
        }
        for (int j = 0; j < nb; j++) {
            const float norm = 0.5f * (e[j + SOF_LEN - 1] - e[j]) +
                               0.5f * (e[j + SOF_LEN] - e[j + 1]) +
                               0.5f * (e[j + HDR_LEN] - e[j + SOF_LEN]);
            const float m = sqrtf(c1r[j] * c1r[j] + c1i[j] * c1i[j]) +
                            sqrtf(c2r[j] * c2r[j] + c2i[j] * c2i[j]);
            if (m > th * (norm + 1e-20f))
                return p0 + j;
        }
    }
    return to;
}

/* Luise & Reggiannini refinement of f0 (rad/symbol) on known symbols. */
static double refine_freq(const cf* y, const cf* ref, int n, double f0)
{
    cf z[HDR_LEN];
    for (int k = 0; k < n; k++)
        z[k] = y[k] * conjf(ref[k]) * cexpf(-I * (float)(f0 * k));
    const int lags = n / 2 < LR_LAGS ? n / 2 : LR_LAGS;
    cf r = 0;
    for (int m = 1; m <= lags; m++)
        for (int k = m; k < n; k++)
            r += z[k] * conjf(z[k - m]);
    return f0 + 2.0 * cargf(r) / (lags + 1);
}

/* Luise & Reggiannini frequency residual (rad/symbol) of derotated
 * symbols z against known symbols ref; *weight receives the inverse of its
 * approximate variance up to a common factor (n^3).
 * sum_{m=1..L} sum_k w[k] conj(w[k-m]) is computed as
 * sum_k w[k] conj(P[k] - P[k - min(L, k)]) with P the prefix sums of w,
 * O(n) instead of O(n L). */
static double lr_residual(const cf* z, const cf* ref, int n, double* weight)
{
    static const cf pilot = 0.70710678f + 0.70710678f * I;
    const int lags = n / 2 < LR_LAGS ? n / 2 : LR_LAGS;
    cf w[HDR_LEN], P[HDR_LEN + 1];
    P[0] = 0;
    for (int k = 0; k < n; k++) {
        w[k] = z[k] * conjf(ref ? ref[k] : pilot);
        P[k + 1] = P[k] + w[k];
    }
    cf r = 0;
    for (int k = 1; k < n; k++)
        r += w[k] * conjf(P[k] - P[k > lags ? k - lags : 0]);
    *weight = (double)n * n * n;
    return 2.0 * cargf(r) / (lags + 1);
}

/* Decode the PLS at y (header start), coherently with frequency f
 * (rad/symbol). Fills ref with the 90 header symbols.
 *
 * The 256 PLS codewords are scored with fast Hadamard transforms rather
 * than 256 correlations of 64 symbols. With c the 32-bit word
 * sum of G[r] w_(7-r) (plsc_encode()), symbol pair (2m, 2m + 1) carries
 * bit i = 31 - m of c and of c ^ w0. Rows 1..5 of G are complemented
 * Walsh functions (bit i of row r is 1 ^ bit (r - 1) of i) and row 6 is
 * all ones, so with t = (w6, w5, w4, w3, w2) as bits 0..4,
 * c_i = w7 G0_i ^ (t.i) ^ parity(t) ^ w1, and the correlation is
 * (-1)^(parity(t) ^ w1) WHT(x)[t], x_i = (-1)^(w7 G0_i) (u_2m + (-1)^w0 u_2m+1)
 * where u are the descrambled soft symbols. */
static int decode_pls(const drx_pl* pl, const cf* y, double f, struct pls* h, cf* ref)
{
    cf yd[HDR_LEN];
    rotate(y, HDR_LEN, 0, -f, 1.0f, NULL, yd);
    cf s = 0;
    for (int k = 0; k < SOF_LEN; k++)
        s += yd[k] * conjf(pl->hdr_sof[k]);
    const cf rot = conjf(s) / (cabsf(s) + 1e-20f);
    /* DVB-S2X codes (>= 128) are sent rotated by 90 degrees, so they are
     * scored on the quadrature part. */
    float soft[2][PLSC_LEN];
    for (int j = 0; j < PLSC_LEN; j++) {
        const int k = SOF_LEN + j;
        const cf v = yd[k] * rot * conjf(pi2_base(k)) * pl->plsc_scr[j];
        soft[0][j] = crealf(v);
        soft[1][j] = cimagf(v);
    }
    float score[256];
    for (int w7 = 0; w7 < 2; w7++) {
        for (int w0 = 0; w0 < 2; w0++) {
            float x[32];
            for (int m = 0; m < 32; m++) {
                const int i = 31 - m;
                const float u = soft[w7][2 * m] + (w0 ? -soft[w7][2 * m + 1] : soft[w7][2 * m + 1]);
                x[i] = w7 && ((PLSC_G0 >> i) & 1) ? -u : u;
            }
            for (int len = 1; len < 32; len *= 2)
                for (int i = 0; i < 32; i += 2 * len)
                    for (int j = i; j < i + len; j++) {
                        const float a = x[j], b = x[j + len];
                        x[j] = a + b;
                        x[j + len] = a - b;
                    }
            for (int t = 0; t < 32; t++) {
                int w = w7 << 7 | w0;
                for (int bit = 0; bit < 5; bit++)
                    w |= ((t >> bit) & 1) << (6 - bit);
                const float sc = __builtin_parity(t) ? -x[t] : x[t];
                score[w] = sc;
                score[w | 2] = -sc;
            }
        }
    }
    int best = 0;
    float best_score = -INFINITY;
    for (int w = 0; w < 256; w++) { /* first maximum, as a scan over w */
        if (score[w] > best_score) {
            best_score = score[w];
            best = w;
        }
    }
    if (parse_pls(best, h))
        return -1;
    memcpy(ref, pl->hdr_sof, sizeof(pl->hdr_sof));
    memcpy(ref + SOF_LEN, pl->hdr_plsc[best], sizeof(pl->hdr_plsc[best]));
    return 0;
}

/* ---- setup -------------------------------------------------------------- */

static int nearest_full(const struct constellation* c, float x, float y)
{
    int best = 0;
    float bd = INFINITY;
    for (int i = 0; i < c->n; i++) {
        const float dx = x - crealf(c->p[i]), dy = y - cimagf(c->p[i]);
        const float d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* A cell belongs to one decision region when its four corners have the
 * same nearest point: regions are convex (intersections of half-planes). */
static void build_grid(struct constellation* c)
{
    float rmax = 0;
    for (int i = 0; i < c->n; i++)
        rmax = cabsf(c->p[i]) > rmax ? cabsf(c->p[i]) : rmax;
    const float lim = 1.25f * rmax, cell = 2 * lim / HARD_GRID;
    c->grid_lo = -lim;
    c->grid_inv = 1 / cell;
    for (int cy = 0; cy < HARD_GRID; cy++) {
        for (int cx = 0; cx < HARD_GRID; cx++) {
            int8_t v = -1;
            if (c->n <= 128) {
                const float x0 = c->grid_lo + cx * cell, y0 = c->grid_lo + cy * cell;
                const int p = nearest_full(c, x0, y0);
                if (nearest_full(c, x0 + cell, y0) == p && nearest_full(c, x0, y0 + cell) == p &&
                    nearest_full(c, x0 + cell, y0 + cell) == p)
                    v = (int8_t)p;
            }
            c->grid[cy * HARD_GRID + cx] = v;
        }
    }
}

static struct constellation* get_constellation(drx_pl* pl, const drx_modcod* m)
{
    struct constellation** c = &pl->cons[m->pls >> 1];
    if (*c)
        return *c;
    drx_demap* d = drx_demap_new(m->constellation, m->rate, m->short_frame);
    if (!d)
        return NULL;
    *c = calloc(1, sizeof(**c));
    float re[256], im[256];
    (*c)->n = drx_demap_points(d, re, im);
    for (int i = 0; i < (*c)->n; i++) {
        (*c)->p[i] = re[i] + im[i] * I;
        (*c)->inv_e[i] = 1.0f / (re[i] * re[i] + im[i] * im[i] + 1e-20f);
    }
    (*c)->dm = d;
    build_grid(*c);
    return *c;
}

/* Block loop filter weights (see frame_pll()): with L the unit lower
 * triangular matrix L_it = K1 + K2 (i - 1 - t) (t < i), e' = L^-1 e; the
 * phase advance K1 sum e' + K2 sum (n - 1 - i) e'_i and the frequency step
 * K2 sum e' are w_phase . e and w_freq . e. */
static void pll_weights(drx_pl* pl)
{
    const int n = PLL_BLOCK;
    for (int j = 0; j < n; j++) {
        double x[PLL_BLOCK]; /* column j of L^-1: solve L x = unit j */
        for (int i = 0; i < n; i++) {
            double v = i == j;
            for (int t = 0; t < i; t++)
                v -= (pl->K1 + pl->K2 * (i - 1 - t)) * x[t];
            x[i] = v;
        }
        double a = 0, b = 0;
        for (int i = 0; i < n; i++) {
            a += x[i];
            b += (n - 1 - i) * x[i];
        }
        pl->w_phase[j] = (float)(pl->K1 * a + pl->K2 * b);
        pl->w_freq[j] = (float)(pl->K2 * a);
    }
}

static void eq_reset(drx_pl* pl)
{
    memset(pl->eq_R, 0, sizeof(pl->eq_R));
    memset(pl->eq_p, 0, sizeof(pl->eq_p));
    for (int m = 0; m < EQ_N; m++)
        pl->eq_w[m] = m == EQ_D ? 1.0f : 0.0f;
    pl->eq_count = pl->eq_phase = 0;
}

drx_pl* drx_pl_new(int gold_code)
{
    drx_pl* pl = calloc(1, sizeof(*pl));
    if (!pl)
        return NULL;
    pl->buf = malloc(sizeof(cf) * CAP);
    pl->descr = malloc(sizeof(cf) * MAX_PAYLOAD);
    pl->out = malloc(sizeof(cf) * MAX_PAYLOAD);
    pl->z = malloc(sizeof(cf) * (MAX_FRAME + SOF_LEN));
    pl->yeq = malloc(sizeof(cf) * (MAX_FRAME + SOF_LEN));
    if (!pl->buf || !pl->descr || !pl->out || !pl->z || !pl->yeq) {
        drx_pl_free(pl);
        return NULL;
    }
    init_descrambler(pl, gold_code);

    const uint32_t sof = 0x18D2E82;
    for (int k = 0; k < SOF_LEN; k++)
        pl->hdr_sof[k] = ((sof >> (SOF_LEN - 1 - k)) & 1 ? -1.0f : 1.0f) * pi2_base(k);
    const uint64_t scrambler = PLSC_SCRAMBLER;
    for (int w = 0; w < 256; w++) {
        uint64_t cw = plsc_encode(w) ^ scrambler;
        const cf rot = (w & 0x80) ? I : 1.0f; /* DVB-S2X: rotated by 90 degrees */
        for (int j = 0; j < PLSC_LEN; j++) {
            float sgn = ((cw >> (63 - j)) & 1) ? -1.0f : 1.0f;
            pl->hdr_plsc[w][j] = sgn * pi2_base(SOF_LEN + j) * rot;
        }
    }
    for (int j = 0; j < PLSC_LEN; j++)
        pl->plsc_scr[j] = ((scrambler >> (63 - j)) & 1) ? -1.0f : 1.0f;
    for (int k = 1; k < SOF_LEN; k++)
        pl->dref_sof[k - 1] = pl->hdr_sof[k] * conjf(pl->hdr_sof[k - 1]);
    for (int i = 0; i < PLSC_LEN / 2; i++)
        pl->dref_plsc[i] = pl->hdr_plsc[0][2 * i + 1] * conjf(pl->hdr_plsc[0][2 * i]);

    pl->shift_pos = -1;
    const char* eq = getenv("DRX_EQ");
    pl->eq_on = !(eq && eq[0] == '0');
    eq_reset(pl);

    /* Phase-locked loop, Bn*T = 2e-3 per symbol, damping 0.707 */
    const double bnt = 2e-3, z = M_SQRT1_2;
    const double th = bnt / (z + 1 / (4 * z)), d = 1 + 2 * z * th + th * th;
    pl->K1 = (float)(4 * z * th / d);
    pl->K2 = (float)(4 * th * th / d);
    pll_weights(pl);
    return pl;
}

void drx_pl_free(drx_pl* pl)
{
    if (!pl)
        return;
    for (int m = 0; m < 128; m++) {
        if (pl->cons[m])
            drx_demap_free(pl->cons[m]->dm);
        free(pl->cons[m]);
    }
    free(pl->buf);
    free(pl->descr);
    free(pl->out);
    free(pl->z);
    free(pl->yeq);
    free(pl);
}

void drx_pl_shift(drx_pl* pl, double delta)
{
    if (pl->shift_pos >= 0) /* previous one not reached yet: apply it now */
        pl->omega -= pl->shift_delta;
    pl->shift_pos = pl->len;
    pl->shift_delta = 2 * M_PI * delta;
}

int drx_pl_shift_pending(const drx_pl* pl) { return pl->shift_pos >= 0; }

void drx_pl_set_out_cb(drx_pl* pl, drx_pl_out_cb get) { pl->get_out = get; }

void drx_pl_reset(drx_pl* pl)
{
    pl->len = pl->pos = 0;
    if (pl->locked)
        pl->stats.lost++;
    pl->locked = 0;
    pl->misses = 0;
    pl->shift_pos = -1;
    eq_reset(pl);
}

void drx_pl_get_stats(const drx_pl* pl, drx_pl_stats* s)
{
    *s = pl->stats;
    s->locked = pl->locked;
}

/* ---- frame processing --------------------------------------------------- */

/* Index of the constellation point nearest to (x, y). */
static inline int decide(const struct constellation* c, float x, float y)
{
    const float fx = (x - c->grid_lo) * c->grid_inv, fy = (y - c->grid_lo) * c->grid_inv;
    if (fx >= 0 && fy >= 0 && fx < HARD_GRID && fy < HARD_GRID) {
        const int p = c->grid[(int)fy * HARD_GRID + (int)fx];
        if (p >= 0)
            return p;
    }
    if (c->n >= 32)
        return drx_demap_nearest(c->dm, x, y);
    return nearest_full(c, x, y);
}

#define PILOT_PERIOD (16 * SLOT_LEN + PILOT_LEN)

static inline int pilot_start(int p) /* frame index of pilot block p */
{
    return HDR_LEN + p * PILOT_PERIOD + 16 * SLOT_LEN;
}

#if defined(__AVX2__) && defined(__FMA__)
/* Four complex products a b, interleaved I/Q. */
static inline __m256 cmul8(__m256 a, __m256 b)
{
    const __m256 as = _mm256_permute_ps(a, 0xB1); /* (im, re) pairs */
    return _mm256_fmaddsub_ps(a, _mm256_moveldup_ps(b), _mm256_mul_ps(as, _mm256_movehdup_ps(b)));
}
#endif

/*
 * out[i] = g y[i] e^{j (ph0 + dph i)}, times descr[i] when descr is not
 * NULL, for i < n. Eight phasors stepped by e^{j 8 dph}: the eight lanes
 * are independent (vectorised), unlike a single phasor stepped once per
 * symbol, whose multiply latency bounded the loop, and there is no sincos
 * per symbol. They are seeded in double precision; rounding drifts by
 * about 1e-7 per step, negligible over the few thousand symbols of a call.
 */
static void rotate(const cf* y, int n, double ph0, double dph, float g, const cf* descr, cf* out)
{
    enum { P = 8 };
    float rr[P], ri[P];
    double complex r = g * cexp(I * ph0);
    const double complex s1 = cexp(I * dph);
    for (int i = 0; i < P; i++) {
        rr[i] = (float)creal(r);
        ri[i] = (float)cimag(r);
        r *= s1;
    }
    double complex s8 = s1 * s1;
    s8 *= s8;
    s8 *= s8;
    const float sr = (float)creal(s8), si = (float)cimag(s8);
    const float* yf = (const float*)y;
    const float* df = (const float*)descr;
    float* o = (float*)out;
    int k = 0;
#if defined(__AVX2__) && defined(__FMA__)
    /* phasors 0..3 and 4..7 as interleaved complex vectors */
    __m256 r0 = _mm256_setr_ps(rr[0], ri[0], rr[1], ri[1], rr[2], ri[2], rr[3], ri[3]);
    __m256 r1 = _mm256_setr_ps(rr[4], ri[4], rr[5], ri[5], rr[6], ri[6], rr[7], ri[7]);
    const __m256 s8v = _mm256_setr_ps(sr, si, sr, si, sr, si, sr, si);
    if (descr) {
        for (; k + P <= n; k += P) {
            const __m256 t0 = cmul8(r0, _mm256_loadu_ps(df + 2 * k));
            const __m256 t1 = cmul8(r1, _mm256_loadu_ps(df + 2 * k + 8));
            _mm256_storeu_ps(o + 2 * k, cmul8(_mm256_loadu_ps(yf + 2 * k), t0));
            _mm256_storeu_ps(o + 2 * k + 8, cmul8(_mm256_loadu_ps(yf + 2 * k + 8), t1));
            r0 = cmul8(r0, s8v);
            r1 = cmul8(r1, s8v);
        }
    } else {
        for (; k + P <= n; k += P) {
            _mm256_storeu_ps(o + 2 * k, cmul8(_mm256_loadu_ps(yf + 2 * k), r0));
            _mm256_storeu_ps(o + 2 * k + 8, cmul8(_mm256_loadu_ps(yf + 2 * k + 8), r1));
            r0 = cmul8(r0, s8v);
            r1 = cmul8(r1, s8v);
        }
    }
    float rv[16];
    _mm256_storeu_ps(rv, r0);
    _mm256_storeu_ps(rv + 8, r1);
    for (int i = 0; i < P; i++) {
        rr[i] = rv[2 * i];
        ri[i] = rv[2 * i + 1];
    }
#else
    if (descr) {
        for (; k + P <= n; k += P) {
            for (int i = 0; i < P; i++) {
                const float dr = df[2 * (k + i)], di = df[2 * (k + i) + 1];
                const float tr = rr[i] * dr - ri[i] * di, ti = rr[i] * di + ri[i] * dr;
                const float xr = yf[2 * (k + i)], xi = yf[2 * (k + i) + 1];
                o[2 * (k + i)] = xr * tr - xi * ti;
                o[2 * (k + i) + 1] = xr * ti + xi * tr;
                const float nr = rr[i] * sr - ri[i] * si;
                ri[i] = rr[i] * si + ri[i] * sr;
                rr[i] = nr;
            }
        }
    } else {
        for (; k + P <= n; k += P) {
            for (int i = 0; i < P; i++) {
                const float xr = yf[2 * (k + i)], xi = yf[2 * (k + i) + 1];
                o[2 * (k + i)] = xr * rr[i] - xi * ri[i];
                o[2 * (k + i) + 1] = xr * ri[i] + xi * rr[i];
                const float nr = rr[i] * sr - ri[i] * si;
                ri[i] = rr[i] * si + ri[i] * sr;
                rr[i] = nr;
            }
        }
    }
#endif
    for (int i = 0; k < n; k++, i++) {
        const cf t = (rr[i] + ri[i] * I) * (descr ? descr[k] : 1.0f);
        out[k] = y[k] * t;
    }
}

/* z[k] = y[k] e^{-j omega k} for k in [start, start + n), descrambled in
 * the payload (HDR_LEN <= k < frame_len). */
static void derotate(const drx_pl* pl, const cf* y, int start, int n, int frame_len, cf* z)
{
    const int chunk = 2048; /* re-seeded from the exact phase */
    const int end = start + n;
    for (int k = start; k < end;) {
        int k1 = k < HDR_LEN ? HDR_LEN : k < frame_len ? frame_len : end;
        k1 = k1 > end ? end : k1 - k > chunk ? k + chunk : k1;
        const int scrambled = k >= HDR_LEN && k < frame_len;
        rotate(y + k, k1 - k, -pl->omega * k, -pl->omega, 1.0f,
               scrambled ? pl->descr + (k - HDR_LEN) : NULL, z + k);
        k = k1;
    }
}

struct block {
    int start, len;
    const cf* ref; /* NULL: unmodulated pilot (1+j)/sqrt(2) */
    double phase, center;
    float mag;
};

/* Sum of |g z[k] e^{-j (ph0 + dph (k - k0))} - ref[k]|^2 over [k0, k1). */
static double known_err(const cf* z, int k0, int k1, double ph0, double dph, float g,
                        const cf* ref)
{
    const cf pilot = (1.0f + 1.0f * I) * (float)M_SQRT1_2;
    cf v[HDR_LEN];
    double err = 0;
    if (k1 <= k0)
        return 0;
    rotate(z + k0, k1 - k0, -ph0, -dph, g, NULL, v);
    for (int k = k0; k < k1; k++) {
        const cf e = v[k - k0] - (ref ? ref[k - k0] : pilot);
        err += crealf(e * conjf(e));
    }
    return err;
}

/*
 * Pilot-aided carrier recovery: the phase is measured on every known block
 * (header, pilot blocks and, when available, the next frame's SOF) and
 * interpolated linearly in between. No decisions on data symbols.
 *
 * Only the known blocks of z are derotated (by omega, used here as
 * omega0, the frequency before track_freq() updated it); the data symbols
 * are taken from y with the omega ramp and the interpolated phase as one
 * linear phase per data run (a run lies between two consecutive known
 * blocks, so within one interpolation segment).
 */
static int frame_pilot_aided(drx_pl* pl, const struct pls* h, const cf* hdr, const cf* y,
                             const cf* z, double omega0, int next_sof, cf* out, double* err,
                             int* n_err)
{
    const cf pilot = (1.0f + 1.0f * I) * (float)M_SQRT1_2;
    struct block b[MAX_PILOTS + 2];
    int nb = 0;
    b[nb++] = (struct block){ .start = 0, .len = HDR_LEN, .ref = hdr };
    for (int p = 0; p < h->npilots; p++)
        b[nb++] = (struct block){ .start = pilot_start(p), .len = PILOT_LEN };
    if (next_sof)
        b[nb++] = (struct block){ .start = h->len, .len = SOF_LEN, .ref = pl->hdr_sof };

    double asum = 0;
    int alen = 0;
    for (int i = 0; i < nb; i++) {
        cf s = 0;
        for (int k = 0; k < b[i].len; k++)
            s += z[b[i].start + k] * conjf(b[i].ref ? b[i].ref[k] : pilot);
        double ph = cargf(s);
        if (i) { /* unwrap against the previous block */
            double d = ph - b[i - 1].phase;
            d -= 2 * M_PI * floor((d + M_PI) / (2 * M_PI));
            ph = b[i - 1].phase + d;
        }
        b[i].phase = ph;
        b[i].center = b[i].start + 0.5 * (b[i].len - 1);
        b[i].mag = cabsf(s) / b[i].len;
        if (b[i].start < h->len) {
            asum += cabsf(s);
            alen += b[i].len;
        }
    }
    const float amp = (float)(asum / alen);
    pl->amp = amp;
    const float inv_amp = 1.0f / amp;

    /* Phase of z at frame index k in segment s: ph[s] + slope[s] k. Segment
     * s spans block centres s and s + 1; the first and the last ones also
     * extend outwards (as the interpolation did symbol by symbol). */
    double ph[MAX_PILOTS + 2], slope[MAX_PILOTS + 2];
    const int nseg = nb > 1 ? nb - 1 : 1;
    for (int s = 0; s < nseg; s++) {
        if (nb == 1) {
            ph[s] = b[0].phase;
            slope[s] = 0;
        } else {
            slope[s] = (b[s + 1].phase - b[s].phase) / (b[s + 1].center - b[s].center);
            ph[s] = b[s].phase - slope[s] * b[s].center;
        }
    }
    for (int s = 0; s < nseg; s++) {
        pl->fr_ph[s] = ph[s];
        pl->fr_slope[s] = slope[s];
    }
    pl->fr_nseg = nseg;
    pl->fr_omega0 = omega0;
    pl->fr_amp = amp;

    /* Known symbols: error energy, split at the block centres. */
    *err += known_err(z, 0, HDR_LEN, ph[0], slope[0], inv_amp, hdr);
    *n_err += HDR_LEN;
    for (int p = 0; p < h->npilots; p++) {
        const int i = p + 1, k0 = b[i].start, k1 = k0 + PILOT_LEN;
        /* before the centre: segment i - 1, after: segment i when it exists */
        const int s0 = i - 1 < nseg ? i - 1 : nseg - 1, s1 = i < nseg ? i : nseg - 1;
        const int kc = k0 + PILOT_LEN / 2; /* first symbol past the centre */
        *err += known_err(z, k0, kc, ph[s0] + slope[s0] * k0, slope[s0], inv_amp, NULL);
        *err += known_err(z, kc, k1, ph[s1] + slope[s1] * kc, slope[s1], inv_amp, NULL);
        *n_err += PILOT_LEN;
    }

    /* Data runs: between the header and pilot block 0, then between pilot
     * blocks, the last one up to the end of the frame. */
    int nout = 0;
    for (int p = 0; p <= h->npilots; p++) {
        const int k0 = HDR_LEN + p * PILOT_PERIOD;
        const int k1 = p < h->npilots ? pilot_start(p) : h->len;
        const int s = p < nseg ? p : nseg - 1;
        const double phase0 = omega0 * k0 + ph[s] + slope[s] * k0;
        rotate(y + k0, k1 - k0, -phase0, -(omega0 + slope[s]), inv_amp, pl->descr + (k0 - HDR_LEN),
               out + nout);
        nout += k1 - k0;
    }
    return nout;
}

/*
 * Decision-directed second-order PLL for frames without pilots, anchored
 * on the header phase and data-aided over the header.
 *
 * The loop runs on blocks of PLL_BLOCK symbols: within a block, symbols
 * are rotated with the phase and frequency at the start of the block and
 * the loop is updated at its end, which takes the decisions off the serial
 * path (a per-symbol loop was bounded by the latency of rotation ->
 * decision -> error -> rotation). The errors are measured without the
 * corrections the per-symbol loop would have made inside the block; to
 * first order e_i = e'_i + sum_{t<i} (K1 + K2 (i - 1 - t)) e'_t, with e'
 * the per-symbol loop's errors. The block update (phase steps adding up to
 * nb dw + K1 sum e'_i + K2 sum (nb - 1 - i) e'_i, frequency step
 * K2 sum e'_i) is linear in e', so in e: precomputed weights (see
 * pll_weights()) give the same loop dynamics as the per-symbol loop, a
 * plain block update degraded 16APSK near threshold.
 */
static int frame_pll(drx_pl* pl, const struct pls* h, const cf* hdr, const cf* z, cf* out,
                     double* err, int* n_err)
{
    const struct constellation* cons = get_constellation(pl, &h->mc);
    if (!cons)
        return 0;
    cf s = 0;
    for (int k = 0; k < HDR_LEN; k++)
        s += z[k] * conjf(hdr[k]);
    const float a_hdr = cabsf(s) / HDR_LEN;
    pl->amp = pl->have_amp ? pl->amp + 0.25f * (a_hdr - pl->amp) : a_hdr;
    const float inv_amp = 1.0f / pl->amp;

    /* z is already derotated by omega: the loop tracks the residual. */
    double dw = 0;
    cf rot = conjf(s) / (cabsf(s) + 1e-20f) * inv_amp; /* e^{-j phi} / amp */
    int nout = 0;
    for (int k0 = 0; k0 < h->len; k0 += PLL_BLOCK) {
        const int nb = h->len - k0 < PLL_BLOCK ? h->len - k0 : PLL_BLOCK;
        /* rotation of symbol k0 + i: rot (1 - j dw)^i ~ rot - j dw i rot */
        const cf drot = -I * (float)dw * rot;
        float e[PLL_BLOCK];
        for (int i = 0; i < nb; i++) {
            const int k = k0 + i;
            const cf v = z[k] * (rot + (float)i * drot);
            cf ref;
            float inv_e;
            if (k < HDR_LEN) {
                ref = hdr[k];
                inv_e = 1.0f;
                const cf d = v - ref;
                *err += crealf(d * conjf(d));
                (*n_err)++;
            } else {
                const int p = decide(cons, crealf(v), cimagf(v));
                ref = cons->p[p];
                inv_e = cons->inv_e[p];
                out[nout++] = v;
            }
            e[i] = cimagf(v * conjf(ref)) * inv_e;
        }
        float sum = (float)(nb * dw), step = 0;
        if (nb == PLL_BLOCK) {
            for (int i = 0; i < PLL_BLOCK; i++) {
                sum += pl->w_phase[i] * e[i];
                step += pl->w_freq[i] * e[i];
            }
        } else { /* last, partial block: the same, solved symbol by symbol */
            float acc = 0, se = 0;
            for (int i = 0; i < nb; i++) {
                const float ei = e[i] - acc; /* e'_i */
                acc += pl->K1 * ei + pl->K2 * se;
                se += ei;
            }
            sum += acc;
            step = pl->K2 * se;
        }
        dw += step;
        rot *= 1.0f - sum * I; /* small-angle e^{-j sum} */
        if ((k0 & 255) == 256 - PLL_BLOCK)
            rot *= inv_amp / cabsf(rot);
    }
    /* Header-only frequency tracking is coarse without pilots; keep part of
     * what the loop learnt for the next frame. */
    pl->omega += 0.5 * dw;
    return nout;
}

/* Frequency tracking: weighted L&R residual over the header and the pilot
 * blocks, which is free of the 1/1476 cycles/symbol ambiguity of the
 * pilot spacing, smoothed across frames. Applies from the next frame on. */
static void track_freq(drx_pl* pl, const struct pls* h, const cf* hdr, const cf* z)
{
    double w, wsum, r = lr_residual(z, hdr, HDR_LEN, &wsum) * wsum;
    for (int p = 0; p < h->npilots; p++) {
        r += lr_residual(z + pilot_start(p), NULL, PILOT_LEN, &w) * w;
        wsum += w;
    }
    const double gain = pl->frames_locked < 8 ? 0.5 : 0.1;
    pl->omega += gain * r / wsum;
    pl->frames_locked++;
}

/* out[k] = sum_m w[m] y[k + m - EQ_D], k in [0, n); y is valid from
 * y[-before] to y[after - 1] (zeros beyond). */
static void eq_apply(const drx_pl* pl, const cf* y, int before, int after, int n, cf* out)
{
    typedef float v8f __attribute__((vector_size(32)));
    const cf* w = pl->eq_w;
    int k = 0;
    /* vector part: 4 symbols at a time, all taps inside the valid range */
    const int k_lo = EQ_D - before > 0 ? EQ_D - before : 0;
    const int k_hi = after - EQ_D < n ? after - EQ_D : n;
    v8f wr[EQ_N], wi[EQ_N];
    for (int m = 0; m < EQ_N; m++) {
        const float a = crealf(w[m]), b = cimagf(w[m]);
        wr[m] = (v8f){ a, a, a, a, a, a, a, a };
        wi[m] = (v8f){ -b, b, -b, b, -b, b, -b, b };
    }
    for (; k < k_lo && k < n; k++) {
        cf acc = 0;
        for (int m = 0; m < EQ_N; m++) {
            const int i = k + m - EQ_D;
            if (i >= -before && i < after)
                acc += w[m] * y[i];
        }
        out[k] = acc;
    }
    for (; k + 4 <= k_hi; k += 4) {
        v8f acc = { 0 };
        const float* yf = (const float*)(y + k - EQ_D);
        for (int m = 0; m < EQ_N; m++) {
            v8f a;
            memcpy(&a, yf + 2 * m, sizeof(a));
            const v8f sw = __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6);
            acc += a * wr[m] + sw * wi[m];
        }
        memcpy((float*)(out + k), &acc, sizeof(acc));
    }
    for (; k < n; k++) {
        cf acc = 0;
        for (int m = 0; m < EQ_N; m++) {
            const int i = k + m - EQ_D;
            if (i >= -before && i < after)
                acc += w[m] * y[i];
        }
        out[k] = acc;
    }
}

/* Least squares update on the known symbols of a pilot frame: y (not
 * equalised) against the reference symbols with the carrier phase and
 * amplitude that frame_pilot_aided() measured on the equalised frame. */
static void eq_train(drx_pl* pl, const cf* y, int before, int after, const struct pls* h,
                     const cf* hdr)
{
    const cf pilot = (1.0f + 1.0f * I) * (float)M_SQRT1_2;
    for (int i = 0; i < EQ_N; i++) {
        pl->eq_p[i] *= EQ_LAMBDA;
        for (int j = i; j < EQ_N; j++)
            pl->eq_R[i][j] *= EQ_LAMBDA;
    }
    const int nb = 1 + h->npilots, nseg = pl->fr_nseg;
    for (int b = 0; b < nb; b++) {
        const int k0 = b ? pilot_start(b - 1) : 0, len = b ? PILOT_LEN : HDR_LEN;
        if (k0 - EQ_D < -before || k0 + len + EQ_D > after)
            continue;
        for (int k = k0; k < k0 + len; k++) {
            int s = 0;
            if (b) { /* as frame_pilot_aided(): split at the block centre */
                s = k < k0 + PILOT_LEN / 2 ? b - 1 : b;
                s = s < nseg ? s : nseg - 1;
            }
            const double phi = pl->fr_omega0 * k + pl->fr_ph[s] + pl->fr_slope[s] * k;
            double complex t = pl->fr_amp * cexp(I * phi);
            t *= b ? pilot * conjf(pl->descr[k - HDR_LEN]) : hdr[k];
            const cf* u = y + k - EQ_D;
            for (int i = 0; i < EQ_N; i++) {
                const double complex ci = conj((double complex)u[i]);
                pl->eq_p[i] += ci * t;
                for (int j = i; j < EQ_N; j++)
                    pl->eq_R[i][j] += ci * u[j];
            }
        }
    }
    /* Cholesky of R (Hermitian, upper triangle stored), lightly loaded */
    double complex L[EQ_N][EQ_N], z[EQ_N], x[EQ_N];
    double tr = 0;
    for (int i = 0; i < EQ_N; i++)
        tr += creal(pl->eq_R[i][i]);
    const double load = 1e-4 * tr / EQ_N;
    for (int i = 0; i < EQ_N; i++) {
        for (int j = 0; j <= i; j++) {
            double complex sum = j == i ? pl->eq_R[i][i] + load : conj(pl->eq_R[j][i]);
            for (int m = 0; m < j; m++)
                sum -= L[i][m] * conj(L[j][m]);
            if (j == i) {
                if (creal(sum) <= 0)
                    return;
                L[i][i] = sqrt(creal(sum));
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    for (int i = 0; i < EQ_N; i++) { /* L z = p */
        double complex sum = pl->eq_p[i];
        for (int m = 0; m < i; m++)
            sum -= L[i][m] * z[m];
        z[i] = sum / L[i][i];
    }
    for (int i = EQ_N - 1; i >= 0; i--) { /* L^H x = z */
        double complex sum = z[i];
        for (int m = i + 1; m < EQ_N; m++)
            sum -= conj(L[m][i]) * x[m];
        x[i] = sum / L[i][i];
    }
    for (int i = 0; i < EQ_N; i++)
        pl->eq_w[i] = (cf)x[i];
}

/* Carrier recovery and output of one PLFRAME starting at y. next_sof: the
 * next frame's SOF follows in the buffer. */
static void process_frame(drx_pl* pl, const cf* y, const struct pls* h, const cf* hdr,
                          int next_sof, drx_pl_cb cb, void* ctx)
{
    pl->index++;
    if (h->dummy) {
        pl->stats.dummies++;
        return;
    }
    const cf* yraw = y;
    const int before = (int)(y - pl->buf), after = (int)(pl->buf + pl->len - y);
    if (pl->eq_on) {
        eq_apply(pl, y, before, after, h->len + (next_sof ? SOF_LEN : 0), pl->yeq);
        y = pl->yeq;
    }
    const double omega0 = pl->omega;
    if (h->pilots) { /* known blocks only, see frame_pilot_aided() */
        derotate(pl, y, 0, HDR_LEN, h->len, pl->z);
        for (int p = 0; p < h->npilots; p++)
            derotate(pl, y, pilot_start(p), PILOT_LEN, h->len, pl->z);
        if (next_sof)
            derotate(pl, y, h->len, SOF_LEN, h->len, pl->z);
    } else {
        derotate(pl, y, 0, h->len, h->len, pl->z);
    }
    track_freq(pl, h, hdr, pl->z);

    const int nsym = h->len - HDR_LEN - h->npilots * PILOT_LEN;
    cf* out = pl->get_out ? (cf*)pl->get_out(ctx, &h->mc, nsym) : NULL;
    if (!out)
        out = pl->out;
    double err = 0;
    int n_err = 0, nout;
    if (h->pilots)
        nout = frame_pilot_aided(pl, h, hdr, y, pl->z, omega0, next_sof, out, &err, &n_err);
    else
        nout = frame_pll(pl, h, hdr, pl->z, out, &err, &n_err);
    pl->have_amp = 1;
    if (!nout)
        return;
    if (pl->eq_on && h->pilots &&
        pl->eq_phase++ % (pl->eq_count < 64 ? EQ_EVERY : EQ_EVERY_SLOW) == 0) {
        eq_train(pl, yraw, before, after, h, hdr);
        pl->eq_count++;
    }

    const float n0 = (float)(err / (n_err ? n_err : 1));
    if (10.0f * log10f(1.0f / (n0 + 1e-20f)) < qef_db(&h->mc) - 4.0f) {
        /* The known symbols are 4 dB below what this MODCOD needs: wrong
         * lock or noise, e.g. after a gap in the input. Do not emit the
         * frame (the LDPC decoder would burn time on it) and count it
         * towards the loss of lock. */
        if (++pl->misses >= MAX_MISSES) {
            pl->locked = 0;
            pl->stats.lost++;
        }
        return;
    }
    pl->n0 = pl->n0 > 0 ? pl->n0 + 0.25f * (n0 - pl->n0) : n0;
    pl->misses = 0;
    pl->stats.frames++;
    drx_plframe info = {
        .modcod = h->mc,
        .pilots = h->pilots,
        .nsym = nout,
        .n0 = pl->n0,
        .snr_db = 10.0f * log10f(1.0f / (pl->n0 + 1e-20f)),
        .freq = (float)(pl->omega / (2 * M_PI)),
        .settled = pl->frames_locked >= 8,
        .index = pl->index,
    };
    cb(ctx, &info, (const float*)out);
}

/* ---- driver ------------------------------------------------------------- */

static void run(drx_pl* pl, drx_pl_cb cb, void* ctx)
{
    cf hdr[HDR_LEN];
    struct pls h;
    for (;;) {
        if (!pl->locked) {
            for (;;) {
                const int to = pl->len - HDR_LEN + 1;
                if (pl->pos >= to)
                    return;
                const int p = scan_hdr(pl, pl->pos, to, TH_DETECT);
                pl->stats.skipped += p - pl->pos;
                pl->pos = p;
                if (p >= to)
                    return;
                cf c;
                const cf* y = pl->buf + pl->pos;
                if (hdr_metric(pl, y, &c) > TH_DETECT) {
                    double f = refine_freq(y, pl->hdr_sof, SOF_LEN, cargf(c));
                    if (!decode_pls(pl, y, f, &h, hdr)) {
                        f = refine_freq(y, hdr, HDR_LEN, f);
                        if (pl->pos + h.len + HDR_LEN > pl->len)
                            return; /* wait for the next header */
                        cf c2;
                        if (hdr_metric(pl, y + h.len, &c2) > TH_CONFIRM) {
                            pl->locked = 1;
                            pl->misses = 0;
                            pl->omega = f;
                            pl->frames_locked = 0;
                            pl->have_amp = 0;
                            pl->n0 = 0;
                            break;
                        }
                    }
                }
                pl->pos++;
                pl->stats.skipped++;
            }
        }

        /* Locked: the header is expected at pos, give or take a slip. */
        if (pl->pos + HDR_LEN + SLIP > pl->len)
            return;
        int best = 0;
        float best_m = -1;
        cf c;
        for (int d = -SLIP; d <= SLIP; d++) {
            if (pl->pos + d < 0)
                continue;
            float m = hdr_metric(pl, pl->buf + pl->pos + d, &c);
            if (m > best_m) {
                best_m = m;
                best = d;
            }
        }
        const cf* y = pl->buf + pl->pos + best;
        /* The tracked frequency is far more accurate than what a single
         * header gives at low SNR, so the PLS is decoded with it. */
        if (best_m < TH_TRACK || decode_pls(pl, y, pl->omega, &h, hdr)) {
            if (++pl->misses >= MAX_MISSES) {
                pl->locked = 0;
                pl->stats.lost++;
            }
            /* skip one minimal frame and look again */
            pl->pos += HDR_LEN;
            continue;
        }
        if (pl->pos + best + h.len + SOF_LEN + SLIP + EQ_D > pl->len)
            return; /* frame and next SOF not complete yet */
        if (pl->shift_pos >= 0 && pl->pos + best >= pl->shift_pos) {
            pl->omega -= pl->shift_delta;
            pl->shift_pos = -1;
        }
        cf c_next;
        const int next_sof = hdr_metric(pl, y + h.len, &c_next) > TH_TRACK;
        process_frame(pl, y, &h, hdr, next_sof, cb, ctx);
        pl->pos += best + h.len;
    }
}

void drx_pl_process(drx_pl* pl, const float* sym, int n, drx_pl_cb cb, void* ctx)
{
    while (n > 0) {
        if (pl->len + n > CAP && pl->pos > SLIP + EQ_KEEP) {
            const int keep_from = pl->pos - SLIP - EQ_KEEP;
            memmove(pl->buf, pl->buf + keep_from, sizeof(cf) * (pl->len - keep_from));
            pl->len -= keep_from;
            pl->pos -= keep_from;
            if (pl->shift_pos >= 0)
                pl->shift_pos = pl->shift_pos > keep_from ? pl->shift_pos - keep_from : 0;
        }
        int m = CAP - pl->len < n ? CAP - pl->len : n;
        if (m <= 0) {
            /* Buffer full without progress: drop the oldest data. */
            pl->pos = pl->len;
            continue;
        }
        memcpy(pl->buf + pl->len, sym, sizeof(cf) * m);
        pl->len += m;
        sym += 2 * m;
        n -= m;
        run(pl, cb, ctx);
    }
}
