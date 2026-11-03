/*
 * DVB-S2 soft demapper and bit deinterleaver, see demap.h.
 *
 * LLRs are max-log: (min distance over points with the bit at 1 - min
 * distance over points with the bit at 0) / n0, which is exact for QPSK.
 * Constellations and interleaver column orders come from const_tables.h,
 * measured on the gr-dtv modulator.
 */
#include "dvbrx/demap.h"

#include <assert.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct const_row {
    int pls; /* PLS code, pilot bit cleared */
    const char* constellation;
    const char* rate;
    int short_frame, bits;
    int order[8];
    float points[256][2];
};
#include "const_tables.h"

#ifndef DEMAP_RING
#define DEMAP_RING 1
#endif
#ifndef LLR_SCALE
#define LLR_SCALE 2.0f
#endif
#define BLK 64 /* symbols demapped together */

#define MAX_BITS 8
#define LUT_GRID 128 /* candidate table cells per axis */
#define MAX_CAND 64

struct drx_demap {
    int bits, npoints, nsym, nldpc;
    int order[MAX_BITS];
    float re[1 << MAX_BITS], im[1 << MAX_BITS];
    float ar[1 << MAX_BITS], ai[1 << MAX_BITS], e[1 << MAX_BITS]; /* -2 re, -2 im, |p|^2 */

    /* Ring constellations (DVB-S2X 256APSK 135/180 and 140/180: 8 rings of
     * the same 32 phases), see minima_ring(): point p is
     * R[p >> ring_pb] (cos, sin)[p & (2^ring_pb - 1)]; ring_pb 0 if not. */
    int ring_pb;
    float rc[32], rs[32];   /* phase unit vectors, by low label bits */
    float rm2[8], rr2[8];   /* -2 R and R^2, by high label bits */

    /* Dense constellations: for each cell of a grid over the I/Q plane,
     * the points that can be the closest one with some bit at some value
     * for a symbol in that cell; used for fast hard decisions
     * (drx_demap_nearest). LLRs use the vectorised full search, faster
     * than walking these lists even for 256 points. */
    float lut_lo, lut_inv; /* cell index = (v - lut_lo) * lut_inv */
    uint32_t* lut_start;   /* LUT_GRID^2 + 1 offsets into lut_cand */
    uint8_t* lut_cand;
};

int drx_pls_lookup(int pls, drx_modcod* m)
{
    pls &= 0xFE;
    for (size_t i = 0; i < sizeof(const_rows) / sizeof(*const_rows); i++) {
        if (const_rows[i].pls == pls) {
            m->pls = pls;
            m->constellation = const_rows[i].constellation;
            m->rate = const_rows[i].rate;
            m->short_frame = const_rows[i].short_frame;
            m->bits = const_rows[i].bits;
            return 0;
        }
    }
    return -1;
}

/* For each (bit, value), the closest point to a symbol y in a cell of
 * centre c and half-diagonal h is within h of the closest one to c, so
 * only points within dmin(c) + 2h of c can win: keep those (exact). */
static int build_lut(drx_demap* d)
{
    float rmax = 0;
    for (int p = 0; p < d->npoints; p++) {
        float r = sqrtf(d->re[p] * d->re[p] + d->im[p] * d->im[p]);
        rmax = r > rmax ? r : rmax;
    }
    const float lim = 1.25f * rmax, cell = 2 * lim / LUT_GRID, h = cell * (float)M_SQRT1_2;
    d->lut_lo = -lim;
    d->lut_inv = 1 / cell;
    const int ncell = LUT_GRID * LUT_GRID;
    d->lut_start = malloc(sizeof(uint32_t) * (ncell + 1));
    d->lut_cand = malloc((size_t)ncell * MAX_CAND);
    if (!d->lut_start || !d->lut_cand)
        return -1;

    uint32_t used = 0;
    float dist[256];
    for (int cy = 0; cy < LUT_GRID; cy++) {
        for (int cx = 0; cx < LUT_GRID; cx++) {
            const float x = d->lut_lo + (cx + 0.5f) * cell, y = d->lut_lo + (cy + 0.5f) * cell;
            for (int p = 0; p < d->npoints; p++)
                dist[p] = sqrtf((x - d->re[p]) * (x - d->re[p]) + (y - d->im[p]) * (y - d->im[p]));
            uint8_t keep[256] = { 0 };
            for (int b = 0; b < d->bits; b++) {
                for (int v = 0; v < 2; v++) {
                    float best = INFINITY;
                    for (int p = 0; p < d->npoints; p++)
                        if (((p >> (d->bits - 1 - b)) & 1) == v && dist[p] < best)
                            best = dist[p];
                    for (int p = 0; p < d->npoints; p++)
                        if (((p >> (d->bits - 1 - b)) & 1) == v && dist[p] <= best + 2 * h)
                            keep[p] = 1;
                }
            }
            const int c = cy * LUT_GRID + cx;
            d->lut_start[c] = used;
            int n = 0;
            for (int p = 0; p < d->npoints; p++)
                n += keep[p];
            if (n > MAX_CAND) { /* rare: search everything for this cell */
                d->lut_start[c] = UINT32_MAX;
                continue;
            }
            for (int p = 0; p < d->npoints; p++)
                if (keep[p])
                    d->lut_cand[used++] = (uint8_t)p;
        }
    }
    d->lut_start[ncell] = used;
    return 0;
}

/* Points that are rings of the same phases, the ring in the high label
 * bits and the phase in the low ones (5 phase bits and 3 ring bits only:
 * the case minima_ring() is compiled for). */
static void detect_rings(drx_demap* d)
{
    const int pb = 5, P = 1 << pb, nr = d->npoints >> pb;
    d->ring_pb = 0;
    if (d->bits != 8)
        return;
    for (int r = 0; r < nr; r++) {
        const float R = hypotf(d->re[r << pb], d->im[r << pb]);
        d->rm2[r] = -2 * R;
        d->rr2[r] = R * R;
    }
    const float R0 = hypotf(d->re[0], d->im[0]);
    for (int j = 0; j < P; j++) {
        d->rc[j] = d->re[j] / R0;
        d->rs[j] = d->im[j] / R0;
    }
    for (int p = 0; p < d->npoints; p++) {
        const float R = -0.5f * d->rm2[p >> pb];
        if (fabsf(R * d->rc[p & (P - 1)] - d->re[p]) > 1e-5f ||
            fabsf(R * d->rs[p & (P - 1)] - d->im[p]) > 1e-5f)
            return;
    }
    d->ring_pb = pb;
}

drx_demap* drx_demap_new(const char* constellation, const char* rate, int short_frame)
{
    static_assert(MAX_BITS <= 8, "candidate indexes are bytes");
    const struct const_row* row = NULL;
    for (size_t i = 0; i < sizeof(const_rows) / sizeof(*const_rows); i++)
        if (!strcmp(const_rows[i].constellation, constellation) &&
            !strcmp(const_rows[i].rate, rate) && const_rows[i].short_frame == short_frame)
            row = &const_rows[i];
    if (!row)
        return NULL;

    drx_demap* d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->bits = row->bits;
    d->npoints = 1 << row->bits;
    d->nldpc = short_frame ? 16200 : 64800;
    /* 128APSK: 64800 bits plus 6 padding bits in 9258 symbols */
    d->nsym = (d->nldpc + d->bits - 1) / d->bits;
    memcpy(d->order, row->order, sizeof(d->order));

    /* Normalise to unit average energy, like the AGC output. */
    double e = 0;
    for (int p = 0; p < d->npoints; p++)
        e += row->points[p][0] * row->points[p][0] + row->points[p][1] * row->points[p][1];
    const float g = (float)(1.0 / sqrt(e / d->npoints));
    for (int p = 0; p < d->npoints; p++) {
        d->re[p] = row->points[p][0] * g;
        d->im[p] = row->points[p][1] * g;
        d->ar[p] = -2 * d->re[p];
        d->ai[p] = -2 * d->im[p];
        d->e[p] = d->re[p] * d->re[p] + d->im[p] * d->im[p];
    }
    detect_rings(d);
    if (d->npoints >= 32 && build_lut(d)) {
        drx_demap_free(d);
        return NULL;
    }
    return d;
}

void drx_demap_free(drx_demap* d)
{
    if (!d)
        return;
    free(d->lut_start);
    free(d->lut_cand);
    free(d);
}

int drx_demap_bits(const drx_demap* d) { return d->bits; }

int drx_demap_symbols(const drx_demap* d) { return d->nsym; }

int drx_demap_points(const drx_demap* d, float* re, float* im)
{
    memcpy(re, d->re, sizeof(float) * d->npoints);
    memcpy(im, d->im, sizeof(float) * d->npoints);
    return d->npoints;
}

static inline int8_t quantise(float v)
{
    v = v > 127.0f ? 127.0f : v < -127.0f ? -127.0f : v;
    return (int8_t)lrintf(v);
}

/* o[i] = quantise(k * (a[i] - b[i])), i < n. GCC does not vectorise the
 * clamps and the conversion (they may trap), so AVX2 does it by hand: the
 * conversion rounds to nearest even, as lrintf. */
static void quantise_diff(int8_t* restrict o, const float* restrict a, const float* restrict b,
                          float k, int n)
{
    int i = 0;
#if defined(__AVX2__)
    const __m256 kv = _mm256_set1_ps(k), hi = _mm256_set1_ps(127.0f), lo = _mm256_set1_ps(-127.0f);
    for (; i + 32 <= n; i += 32) {
        __m256i q[4];
        for (int j = 0; j < 4; j++) {
            const __m256 vb = b ? _mm256_loadu_ps(b + i + 8 * j) : _mm256_setzero_ps();
            __m256 v = _mm256_mul_ps(kv, _mm256_sub_ps(_mm256_loadu_ps(a + i + 8 * j), vb));
            v = _mm256_min_ps(_mm256_max_ps(v, lo), hi);
            q[j] = _mm256_cvtps_epi32(v);
        }
        /* packs work within 128-bit lanes: restore the order at the end */
        const __m256i p =
            _mm256_packs_epi16(_mm256_packs_epi32(q[0], q[1]), _mm256_packs_epi32(q[2], q[3]));
        const __m256i order = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
        _mm256_storeu_si256((__m256i*)(o + i), _mm256_permutevar8x32_epi32(p, order));
    }
#endif
    for (; i < n; i++)
        o[i] = quantise(k * (a[i] - (b ? b[i] : 0.0f)));
}

/* Bit b of symbols j0..j0+n goes to codeword position order[b] * nsym + j;
 * positions past the codeword (128APSK padding) are dropped. */
static void store_llrs(const drx_demap* d, int b, int j0, int n, float k, const float* m0,
                       const float* m1, int8_t* llr)
{
    const int base = d->order[b] * d->nsym + j0;
    if (base + n > d->nldpc)
        n = d->nldpc - base > 0 ? d->nldpc - base : 0;
    quantise_diff(llr + base, m1, m0, k, n);
}

/* Per-bit minima over the points, m0[b][i] (bit b of the label at 0) and
 * m1[b][i] (at 1), of the distance from symbol (x[i], y[i]) minus |y|^2
 * (common to all points, so it cancels in the LLR):
 * |p|^2 - 2 Re(y conj(p)), two multiply-adds per point.
 *
 * The minima are taken hierarchically, from the least significant label
 * bit up: the minima over even and odd labels give that bit, then pairs
 * are merged and the next bit comes from the merged table (about 3 np
 * comparisons instead of np bits). The per-symbol loops are what the
 * compiler vectorises. */
static void minima_table(const drx_demap* d, const float* x, const float* y, float m0[][BLK],
                         float m1[][BLK])
{
    static_assert(BLK * (1 << MAX_BITS) * sizeof(float) <= 128 * 1024, "stack table");
    float t[1 << MAX_BITS][BLK];
    for (int p = 0; p < d->npoints; p++) {
        const float ar = d->ar[p], ai = d->ai[p], e = d->e[p];
        for (int i = 0; i < BLK; i++)
            t[p][i] = x[i] * ar + (y[i] * ai + e);
    }
    for (int b = d->bits - 1, cur = d->npoints; b >= 0; b--, cur /= 2) {
        float* restrict lo = m0[b];
        float* restrict hi = m1[b];
        for (int i = 0; i < BLK; i++) {
            lo[i] = t[0][i];
            hi[i] = t[1][i];
            t[0][i] = lo[i] < hi[i] ? lo[i] : hi[i];
        }
        for (int q = 2; q < cur; q += 2) {
            float* restrict a = t[q];
            float* restrict c = t[q + 1];
            float* restrict o = t[q / 2];
            for (int i = 0; i < BLK; i++) {
                lo[i] = a[i] < lo[i] ? a[i] : lo[i];
                hi[i] = c[i] < hi[i] ? c[i] : hi[i];
                o[i] = a[i] < c[i] ? a[i] : c[i];
            }
        }
    }
}

#if defined(__AVX2__) && defined(__FMA__)
/* The same with the whole tree in registers, eight symbols at a time:
 * the table version is bound by its stores (one per comparison). Each
 * distance enters the tree like a binary counter: at level L (label bit
 * nb - 1 - L) an even index updates the bit-0 minimum and waits for its
 * pair, an odd one updates the bit-1 minimum and carries the pair's
 * minimum to the next level. Same comparisons, same results. */
static inline __attribute__((always_inline)) void minima_avx(const drx_demap* d, const int nb,
                                                             const float* x, const float* y,
                                                             float m0[][BLK], float m1[][BLK])
{
    const int np = 1 << nb;
    for (int i = 0; i < BLK; i += 8) {
        const __m256 X = _mm256_loadu_ps(x + i), Y = _mm256_loadu_ps(y + i);
        __m256 lo[MAX_BITS], hi[MAX_BITS], wait[MAX_BITS];
#pragma GCC unroll 32
        for (int p = 0; p < np; p++) {
            __m256 v = _mm256_fmadd_ps(X, _mm256_broadcast_ss(&d->ar[p]),
                                       _mm256_fmadd_ps(Y, _mm256_broadcast_ss(&d->ai[p]),
                                                       _mm256_broadcast_ss(&d->e[p])));
            int idx = p;
#pragma GCC unroll 8
            for (int L = 0; L < nb; L++) {
                const int b = nb - 1 - L;
                if (!(idx & 1)) {
                    lo[b] = idx ? _mm256_min_ps(v, lo[b]) : v;
                    wait[L] = v;
                    break;
                }
                hi[b] = idx > 1 ? _mm256_min_ps(v, hi[b]) : v;
                v = _mm256_min_ps(wait[L], v);
                idx >>= 1;
            }
        }
        for (int b = 0; b < nb; b++) {
            _mm256_storeu_ps(m0[b] + i, lo[b]);
            _mm256_storeu_ps(m1[b] + i, hi[b]);
        }
    }
}
#endif

#if defined(__AVX2__) && defined(__FMA__)
/* Dense constellations (64 to 256 points), eight symbols at a time, same
 * minima as minima_table (min is order-free: identical LLRs). minima_avx
 * chains every point into the low-bit minima, a dependency chain of np / 2
 * minimums (about 64 cycles a symbol for 256 points). Here the points go
 * by groups of 16: within a group, the minima of the 4 low label bits come
 * from a shallow tree, groups are independent (the core overlaps them), and
 * the high bits come from the 16-point minima of the groups. */
static inline __attribute__((always_inline)) __m256 min8(const __m256* v, int stride)
{
    const __m256 a = _mm256_min_ps(v[0], v[stride]), b = _mm256_min_ps(v[2 * stride], v[3 * stride]);
    const __m256 c = _mm256_min_ps(v[4 * stride], v[5 * stride]), e = _mm256_min_ps(v[6 * stride], v[7 * stride]);
    return _mm256_min_ps(_mm256_min_ps(a, b), _mm256_min_ps(c, e));
}

static inline __attribute__((always_inline)) void minima_grp(const drx_demap* d, const int nb,
                                                             const float* x, const float* y,
                                                             float m0[][BLK], float m1[][BLK])
{
    const int ng = 1 << (nb - 4);
    for (int i = 0; i < BLK; i += 8) {
        const __m256 X = _mm256_loadu_ps(x + i), Y = _mm256_loadu_ps(y + i);
        __m256 lo[4], hi[4], gmin[16];
        for (int g = 0; g < ng; g++) {
            __m256 v[16];
#pragma GCC unroll 16
            for (int q = 0; q < 16; q++) {
                const int p = 16 * g + q;
                v[q] = _mm256_fmadd_ps(X, _mm256_broadcast_ss(&d->ar[p]),
                                       _mm256_fmadd_ps(Y, _mm256_broadcast_ss(&d->ai[p]),
                                                       _mm256_broadcast_ss(&d->e[p])));
            }
            /* label bit L of q (L = 0 is the label's LSB, bit nb - 1) */
            const __m256 l0 = min8(v, 2), h0 = min8(v + 1, 2);
            __m256 pr[8];
            for (int k = 0; k < 8; k++)
                pr[k] = _mm256_min_ps(v[2 * k], v[2 * k + 1]);
            const __m256 l1 = _mm256_min_ps(_mm256_min_ps(pr[0], pr[2]), _mm256_min_ps(pr[4], pr[6]));
            const __m256 h1 = _mm256_min_ps(_mm256_min_ps(pr[1], pr[3]), _mm256_min_ps(pr[5], pr[7]));
            const __m256 q0 = _mm256_min_ps(pr[0], pr[1]), q1 = _mm256_min_ps(pr[2], pr[3]);
            const __m256 q2 = _mm256_min_ps(pr[4], pr[5]), q3 = _mm256_min_ps(pr[6], pr[7]);
            const __m256 l2 = _mm256_min_ps(q0, q2), h2 = _mm256_min_ps(q1, q3);
            const __m256 l3 = _mm256_min_ps(q0, q1), h3 = _mm256_min_ps(q2, q3);
            gmin[g] = _mm256_min_ps(l3, h3);
            if (g == 0) {
                lo[0] = l0, hi[0] = h0, lo[1] = l1, hi[1] = h1;
                lo[2] = l2, hi[2] = h2, lo[3] = l3, hi[3] = h3;
            } else {
                lo[0] = _mm256_min_ps(lo[0], l0), hi[0] = _mm256_min_ps(hi[0], h0);
                lo[1] = _mm256_min_ps(lo[1], l1), hi[1] = _mm256_min_ps(hi[1], h1);
                lo[2] = _mm256_min_ps(lo[2], l2), hi[2] = _mm256_min_ps(hi[2], h2);
                lo[3] = _mm256_min_ps(lo[3], l3), hi[3] = _mm256_min_ps(hi[3], h3);
            }
        }
        for (int L = 0; L < 4; L++) {
            _mm256_storeu_ps(m0[nb - 1 - L] + i, lo[L]);
            _mm256_storeu_ps(m1[nb - 1 - L] + i, hi[L]);
        }
        /* high label bits L = 4 .. nb - 1: bit L - 4 of the group index */
        for (int L = 4; L < nb; L++) {
            const int s = 1 << (L - 4);
            __m256 a = _mm256_set1_ps(INFINITY), c = _mm256_set1_ps(INFINITY);
            for (int g = 0; g < ng; g++) {
                if (g & s)
                    c = _mm256_min_ps(c, gmin[g]);
                else
                    a = _mm256_min_ps(a, gmin[g]);
            }
            _mm256_storeu_ps(m0[nb - 1 - L] + i, a);
            _mm256_storeu_ps(m1[nb - 1 - L] + i, c);
        }
    }
}
#endif

#if defined(__AVX2__) && defined(__FMA__)
/* Per-bit minima over the bits of an index (bit L of the label is bit
 * L - L0 of the index): minima of the even and odd entries, then the pairs
 * merged for the next bit (t is overwritten). */
static inline __attribute__((always_inline)) void bit_tree(__m256* t, int n, int nb, int L0, int i,
                                                           float m0[][BLK], float m1[][BLK])
{
    for (int L = L0; n > 1; L++, n /= 2) {
        __m256 lo = t[0], hi = t[1];
        for (int k = 2; k < n; k += 2) {
            lo = _mm256_min_ps(lo, t[k]);
            hi = _mm256_min_ps(hi, t[k + 1]);
        }
        _mm256_storeu_ps(m0[nb - 1 - L] + i, lo);
        _mm256_storeu_ps(m1[nb - 1 - L] + i, hi);
        for (int k = 0; k < n / 2; k++)
            t[k] = _mm256_min_ps(t[2 * k], t[2 * k + 1]);
    }
}

/*
 * Ring constellations, same minima as the full search up to rounding: the
 * metric of ring r, phase j is R_r^2 - 2 R_r c_j with c_j the projection
 * of the symbol on phase j. So the minimum over phases for a ring (all
 * that the ring bits need) is at the largest projection, and the minimum
 * over rings for a phase is the lower envelope of 8 lines at c_j: 32
 * projections and 256 multiply-adds per symbol, instead of 512, with 30 %
 * of the comparisons of minima_grp (2 times faster for 256APSK).
 */
static inline __attribute__((always_inline)) void minima_ring(const drx_demap* d, const float* x,
                                                              const float* y, float m0[][BLK],
                                                              float m1[][BLK])
{
    enum { PB = 5, P = 1 << PB, NR = 8, NB = 8 };
    for (int i = 0; i < BLK; i += 8) {
        const __m256 X = _mm256_loadu_ps(x + i), Y = _mm256_loadu_ps(y + i);
        __m256 pm[P], rm[NR], cmax = _mm256_set1_ps(-INFINITY);
        for (int j0 = 0; j0 < P; j0 += 4) {
            __m256 c[4], a[4];
            for (int k = 0; k < 4; k++) {
                c[k] = _mm256_fmadd_ps(X, _mm256_broadcast_ss(&d->rc[j0 + k]),
                                       _mm256_mul_ps(Y, _mm256_broadcast_ss(&d->rs[j0 + k])));
                cmax = _mm256_max_ps(cmax, c[k]);
            }
#pragma GCC unroll 8
            for (int r = 0; r < NR; r++) {
                const __m256 A = _mm256_broadcast_ss(&d->rm2[r]), B = _mm256_broadcast_ss(&d->rr2[r]);
                for (int k = 0; k < 4; k++) {
                    const __m256 t = _mm256_fmadd_ps(c[k], A, B);
                    a[k] = r ? _mm256_min_ps(a[k], t) : t;
                }
            }
            for (int k = 0; k < 4; k++)
                pm[j0 + k] = a[k];
        }
        for (int r = 0; r < NR; r++)
            rm[r] = _mm256_fmadd_ps(cmax, _mm256_broadcast_ss(&d->rm2[r]), _mm256_broadcast_ss(&d->rr2[r]));
        bit_tree(pm, P, NB, 0, i, m0, m1);
        bit_tree(rm, NR, NB, PB, i, m0, m1);
    }
}
#endif

void drx_demap_frame(const drx_demap* d, const float* sym, float n0, int8_t* llr)
{
    const float k = LLR_SCALE / n0;
    const int nb = d->bits, nsym = d->nsym;

    if (d->order[0] < 0) {
        /* QPSK, no interleaver: LLR(b) = 4 x/sqrt(2) / n0 on each axis */
        const float kq = k * 4.0f * (float)M_SQRT1_2;
        quantise_diff(llr, sym, NULL, kq, 2 * nsym);
        return;
    }

    /* Blocks of BLK symbols, see minima_table(). */
    for (int j0 = 0; j0 < nsym; j0 += BLK) {
        const int n = nsym - j0 < BLK ? nsym - j0 : BLK;
        float x[BLK], y[BLK], m0[MAX_BITS][BLK], m1[MAX_BITS][BLK];
        for (int i = 0; i < BLK; i++) {
            x[i] = i < n ? sym[2 * (j0 + i)] : 0.0f;
            y[i] = i < n ? sym[2 * (j0 + i) + 1] : 0.0f;
        }
#if defined(__AVX2__) && defined(__FMA__)
        if (nb == 3)
            minima_avx(d, 3, x, y, m0, m1);
        else if (nb == 4)
            minima_avx(d, 4, x, y, m0, m1);
        else if (nb == 5)
            minima_avx(d, 5, x, y, m0, m1);
        else if (nb == 6)
            minima_grp(d, 6, x, y, m0, m1);
        else if (nb == 7)
            minima_grp(d, 7, x, y, m0, m1);
        else if (nb == 8 && d->ring_pb == 5 && DEMAP_RING)
            minima_ring(d, x, y, m0, m1);
        else if (nb == 8)
            minima_grp(d, 8, x, y, m0, m1);
        else
#endif
            minima_table(d, x, y, m0, m1);
        for (int b = 0; b < nb; b++)
            store_llrs(d, b, j0, n, k, m0[b], m1[b], llr);
    }
}

int drx_demap_nearest(const drx_demap* d, float x, float y)
{
    const uint8_t* cand = NULL;
    int count = d->npoints;
    if (d->lut_start) {
        int cx = (int)((x - d->lut_lo) * d->lut_inv), cy = (int)((y - d->lut_lo) * d->lut_inv);
        cx = cx < 0 ? 0 : cx >= LUT_GRID ? LUT_GRID - 1 : cx;
        cy = cy < 0 ? 0 : cy >= LUT_GRID ? LUT_GRID - 1 : cy;
        const int c = cy * LUT_GRID + cx;
        if (d->lut_start[c] != UINT32_MAX) {
            uint32_t end = d->lut_start[c + 1];
            for (int e = c + 1; end == UINT32_MAX; e++)
                end = d->lut_start[e + 1];
            cand = d->lut_cand + d->lut_start[c];
            count = (int)(end - d->lut_start[c]);
        }
    }
    int best = 0;
    float bd = INFINITY;
    for (int q = 0; q < count; q++) {
        const int p = cand ? cand[q] : q;
        const float dx = x - d->re[p], dy = y - d->im[p], dist = dx * dx + dy * dy;
        if (dist < bd) {
            bd = dist;
            best = p;
        }
    }
    return best;
}

float drx_demap_estimate_n0(const drx_demap* d, const float* sym, int nsym)
{
    double err = 0;
    for (int j = 0; j < nsym; j++) {
        const float x = sym[2 * j], y = sym[2 * j + 1];
        float best = INFINITY;
        for (int p = 0; p < d->npoints; p++) {
            const float dx = x - d->re[p], dy = y - d->im[p];
            const float dist = dx * dx + dy * dy;
            best = dist < best ? dist : best;
        }
        err += best;
    }
    return nsym ? (float)(err / nsym) : 0.0f;
}
