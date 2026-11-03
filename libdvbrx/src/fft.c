/*
 * FFT of a power-of-two size, see fft.h.
 *
 * Decimation in time. From 64 points: a first radix-8 (odd log2 n) or
 * radix-4 pass reads the input in bit-reversed order into the work buffer,
 * then radix-4 passes of span h = R, 4R ... n / 4 complete it (the last one
 * writes back to x). A radix-4 butterfly is two radix-2 stages: it takes
 * a0..a3 = x[k], x[k + h], x[k + 2h], x[k + 3h] of a block of 4h and with
 * W = e^(-2 pi j / 4h):
 *     p1 = W^2k a1, p2 = W^k a2, p3 = W^3k a3
 *     y[k] = a0 + p1 + (p2 + p3)     y[k + 2h] = a0 + p1 - (p2 + p3)
 *     y[k + h] = a0 - p1 - j (p2 - p3)   y[k + 3h] = a0 - p1 + j (p2 - p3)
 * Everything runs 4 complex values at a time in 256-bit vectors (GCC vector
 * extensions: AVX on x86-64, generic code elsewhere): 4 butterflies of
 * consecutive k in the radix-4 passes, whose twiddles are stored expanded
 * (real part twice, imaginary part twice with signs - +) so that a complex
 * product is one swap, one multiply and one FMA; 4 blocks of the first
 * pass, whose sources are then 4 consecutive values (x[r .. r + 3]), and
 * whose results are transposed to their 4 blocks.
 * Below 64 points: the plain iterative radix-2 FFT.
 */
#include "fft.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef float complex cf;
typedef float v8f __attribute__((vector_size(32)));
typedef int32_t v8i __attribute__((vector_size(32)));
typedef int64_t v4l __attribute__((vector_size(32)));

static inline v8f ld(const cf* p)
{
    v8f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void st(cf* p, v8f v)
{
    memcpy(p, &v, sizeof(v));
}

/* (re, im) -> (im, re) in each complex value */
static inline v8f swap_ri(v8f a)
{
    return __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6);
}

/* sign bit of the imaginary parts */
static const v8i NEG_IM = { 0, INT32_MIN, 0, INT32_MIN, 0, INT32_MIN, 0, INT32_MIN };

/* -j a = (im, -re) */
static inline v8f mul_mj(v8f a)
{
    return (v8f)((v8i)swap_ri(a) ^ NEG_IM);
}

/* a w, w given as (wr, wr ..) and (-wi, wi ..) */
static inline v8f cmul(v8f a, v8f wr, v8f wis)
{
    return a * wr + swap_ri(a) * wis;
}

/* 4 x 4 transpose of complex values: v[i] lane j <-> v[j] lane i */
static inline void transpose4(v8f* v)
{
    const v4l a = (v4l)v[0], b = (v4l)v[1], c = (v4l)v[2], d = (v4l)v[3];
    const v4l t0 = __builtin_shufflevector(a, b, 0, 4, 2, 6), t1 = __builtin_shufflevector(a, b, 1, 5, 3, 7);
    const v4l t2 = __builtin_shufflevector(c, d, 0, 4, 2, 6), t3 = __builtin_shufflevector(c, d, 1, 5, 3, 7);
    v[0] = (v8f)__builtin_shufflevector(t0, t2, 0, 1, 4, 5);
    v[1] = (v8f)__builtin_shufflevector(t1, t3, 0, 1, 4, 5);
    v[2] = (v8f)__builtin_shufflevector(t0, t2, 2, 3, 6, 7);
    v[3] = (v8f)__builtin_shufflevector(t1, t3, 2, 3, 6, 7);
}

static int bitrev(int i, int bits)
{
    int r = 0;
    for (int b = 0; b < bits; b++)
        r |= ((i >> b) & 1) << (bits - 1 - b);
    return r;
}

int fft_init(struct fft* f, int n)
{
    memset(f, 0, sizeof(*f));
    f->n = n;
    while ((1 << f->log2n) < n)
        f->log2n++;
    if ((1 << f->log2n) != n)
        return -1;
    if (n < 64) {
        f->tw = malloc(sizeof(*f->tw) * (n / 2 + 1));
        f->rev = malloc(sizeof(*f->rev) * n);
        if (!f->tw || !f->rev) {
            fft_free(f);
            return -1;
        }
        for (int k = 0; k < n / 2; k++)
            f->tw[k] = (cf)cexp(-2 * M_PI * I * k / n);
        for (int i = 0; i < n; i++)
            f->rev[i] = bitrev(i, f->log2n);
        return 0;
    }
    const int R = f->log2n & 1 ? 8 : 4, m = f->log2n - (R == 8 ? 3 : 2);
    f->r0 = R;
    /* first pass: source x[r ..], r = 4 i, holds block rev_m(r) */
    f->rev = malloc(sizeof(*f->rev) * (n / R / 4));
    /* radix-4 passes: per 4 k, W^2k, W^k, W^3k as (wr wr ..) and (-wi wi ..) */
    size_t ntw = 0;
    for (int h = R; h < n; h *= 4)
        ntw += (size_t)h / 4 * 6 * 8;
    f->tw4 = aligned_alloc(32, sizeof(float) * ntw);
    f->tmp = aligned_alloc(32, sizeof(cf) * n);
    if (!f->rev || !f->tw4 || !f->tmp) {
        fft_free(f);
        return -1;
    }
    for (int i = 0; i < n / R / 4; i++)
        f->rev[i] = bitrev(4 * i, m);
    float* t = f->tw4;
    for (int h = R; h < n; h *= 4)
        for (int k = 0; k < h; k += 4) {
            static const int MUL[3] = { 2, 1, 3 };
            for (int s = 0; s < 3; s++, t += 16)
                for (int l = 0; l < 4; l++) {
                    /* exact reduction of the angle before the cosine */
                    const long e = (long)MUL[s] * (k + l) % (4 * h);
                    const double a = -2 * M_PI * (double)e / (4 * h);
                    t[2 * l] = t[2 * l + 1] = (float)cos(a);
                    t[8 + 2 * l] = -(float)sin(a);
                    t[8 + 2 * l + 1] = (float)sin(a);
                }
        }
    return 0;
}

void fft_free(struct fft* f)
{
    free(f->tw);
    free(f->rev);
    free(f->tw4);
    free(f->tmp);
    f->tw = NULL;
    f->rev = NULL;
    f->tw4 = NULL;
    f->tmp = NULL;
}

static void fft_radix2(const struct fft* f, cf* x, int inverse)
{
    const int n = f->n;
    for (int i = 0; i < n; i++) {
        const int r = f->rev[i];
        if (r > i) {
            const cf t = x[i];
            x[i] = x[r];
            x[r] = t;
        }
    }
    for (int half = 1; half < n; half <<= 1) {
        const int step = n / (2 * half);
        for (int base = 0; base < n; base += 2 * half) {
            cf* a = x + base;
            cf* b = a + half;
            for (int k = 0; k < half; k++) {
                cf w = f->tw[k * step];
                if (inverse)
                    w = conjf(w);
                const cf t = b[k] * w;
                b[k] = a[k] - t;
                a[k] += t;
            }
        }
    }
}

/* First pass, radix 4: block g (output d[4g .. 4g + 3]) is the DFT of
 * x[r], x[r + 2q], x[r + q], x[r + 3q], r = rev(g), q = n / 4. */
static void first4(const struct fft* f, const cf* x, cf* d)
{
    const int n = f->n, q = n / 4, M = n / 4;
    for (int i = 0; i < n / 16; i++) {
        const cf* s = x + 4 * i;
        const v8f a0 = ld(s), a1 = ld(s + 2 * q), a2 = ld(s + q), a3 = ld(s + 3 * q);
        const v8f b0 = a0 + a1, b1 = a0 - a1, b2 = a2 + a3, b3 = mul_mj(a2 - a3);
        v8f o[4] = { b0 + b2, b1 + b3, b0 - b2, b1 - b3 };
        transpose4(o);
        /* lanes r + 0, 1, 2, 3 are the blocks g + 0, M / 2, M / 4, 3 M / 4 */
        const int g = f->rev[i];
        st(d + 4 * g, o[0]);
        st(d + 4 * (g + M / 2), o[1]);
        st(d + 4 * (g + M / 4), o[2]);
        st(d + 4 * (g + 3 * M / 4), o[3]);
    }
}

/* First pass, radix 8: block g is the DFT of x[r + rev3(j) q], j < 8,
 * r = rev(g), q = n / 8. */
static void first8(const struct fft* f, const cf* x, cf* d)
{
    const int n = f->n, q = n / 8, M = n / 8;
    const v8f s = (v8f){ 1, 1, 1, 1, 1, 1, 1, 1 } * (float)M_SQRT1_2;
    for (int i = 0; i < n / 32; i++) {
        const cf* p = x + 4 * i;
        const v8f a0 = ld(p), a1 = ld(p + 4 * q), a2 = ld(p + 2 * q), a3 = ld(p + 6 * q);
        const v8f a4 = ld(p + q), a5 = ld(p + 5 * q), a6 = ld(p + 3 * q), a7 = ld(p + 7 * q);
        const v8f b0 = a0 + a1, b1 = a0 - a1, b2 = a2 + a3, b3 = mul_mj(a2 - a3);
        const v8f b4 = a4 + a5, b5 = a4 - a5, b6 = a6 + a7, b7 = mul_mj(a6 - a7);
        const v8f c0 = b0 + b2, c1 = b1 + b3, c2 = b0 - b2, c3 = b1 - b3;
        const v8f c4 = b4 + b6, c5 = b5 + b7, c6 = b4 - b6, c7 = b5 - b7;
        /* W8^1 = (1 - j) / sqrt 2, W8^2 = -j, W8^3 = (-1 - j) / sqrt 2 */
        const v8f t5 = (c5 + mul_mj(c5)) * s, t6 = mul_mj(c6), t7 = (mul_mj(c7) - c7) * s;
        v8f lo[4] = { c0 + c4, c1 + t5, c2 + t6, c3 + t7 };
        v8f hi[4] = { c0 - c4, c1 - t5, c2 - t6, c3 - t7 };
        transpose4(lo);
        transpose4(hi);
        const int g = f->rev[i];
        const int gs[4] = { g, g + M / 2, g + M / 4, g + 3 * M / 4 };
        for (int l = 0; l < 4; l++) {
            st(d + 8 * gs[l], lo[l]);
            st(d + 8 * gs[l] + 4, hi[l]);
        }
    }
}

/* Radix-4 pass of span h, src to dst (may be the same). */
static void pass4(const cf* src, cf* dst, int n, int h, const float* tw)
{
    for (int base = 0; base < n; base += 4 * h) {
        const float* t = tw;
        for (int k = 0; k < h; k += 4, t += 48) {
            const cf* s = src + base + k;
            cf* d = dst + base + k;
            v8f w[6];
            memcpy(w, t, sizeof(w));
            const v8f a0 = ld(s);
            const v8f p1 = cmul(ld(s + h), w[0], w[1]);
            const v8f p2 = cmul(ld(s + 2 * h), w[2], w[3]);
            const v8f p3 = cmul(ld(s + 3 * h), w[4], w[5]);
            const v8f b0 = a0 + p1, b1 = a0 - p1, c0 = p2 + p3, c1 = mul_mj(p2 - p3);
            st(d, b0 + c0);
            st(d + h, b1 + c1);
            st(d + 2 * h, b0 - c0);
            st(d + 3 * h, b1 - c1);
        }
    }
}

static void conj_all(cf* x, int n)
{
    for (int i = 0; i < n; i += 4)
        st(x + i, (v8f)((v8i)ld(x + i) ^ NEG_IM));
}

void fft_run(const struct fft* f, cf* x, int inverse)
{
    if (!f->r0) {
        fft_radix2(f, x, inverse);
        return;
    }
    const int n = f->n;
    /* the inverse is the conjugate of the forward transform of the conjugate */
    if (inverse)
        conj_all(x, n);
    if (f->r0 == 8)
        first8(f, x, f->tmp);
    else
        first4(f, x, f->tmp);
    const float* tw = f->tw4;
    for (int h = f->r0; h < n; tw += (size_t)h / 4 * 48, h *= 4)
        pass4(f->tmp, 4 * h == n ? x : f->tmp, n, h, tw);
    if (inverse)
        conj_all(x, n);
}
