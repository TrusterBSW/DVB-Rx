/*
 * K = 7 convolutional code and Viterbi decoder, see conv.h.
 *
 * Register r = (input << 6) | state, bit 6 the current bit, bit 0 the oldest;
 * X = parity(r & 0171), Y = parity(r & 0133). Both codes tap the current and
 * the oldest bit, so states 2j and 2j+1 feed states j and j+32 through
 * complementary outputs: a butterfly with branch metrics +a / -a.
 */
#include "conv.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

const struct conv_punct CONV_PUNCT[6] = {
    { 0 },
    { 1, 2, { 0, 1 }, { 0, 0 } },                                     /* X1 Y1 */
    { 2, 3, { 0, 1, 1 }, { 0, 0, 1 } },                               /* X1 Y1 Y2 */
    { 3, 4, { 0, 1, 1, 0 }, { 0, 0, 1, 2 } },                         /* X1 Y1 Y2 X3 */
    { 5, 6, { 0, 1, 1, 0, 1, 0 }, { 0, 0, 1, 2, 3, 4 } },             /* X1 Y1 Y2 X3 Y4 X5 */
    { 7, 8, { 0, 1, 1, 1, 1, 0, 1, 0 }, { 0, 0, 1, 2, 3, 4, 5, 6 } }, /* ... Y6 X7 */
};

/* One kept bit at a time: used to reach a period boundary and for the tail. */
static int depuncture_slow(struct conv_depunct* dp, const int16_t* soft, int n, int16_t (*pairs)[2])
{
    const struct conv_punct* p = dp->p;
    int np = 0;
    for (int i = 0; i < n; i++) {
        dp->cur[p->idx[dp->pos]][p->isy[dp->pos]] = soft[i];
        if (++dp->pos == p->n) {
            for (int j = 0; j < p->k; j++) {
                pairs[np][0] = dp->cur[j][0];
                pairs[np][1] = dp->cur[j][1];
                dp->cur[j][0] = dp->cur[j][1] = 0;
                np++;
            }
            dp->pos = 0;
        }
    }
    return np;
}

#ifdef __SSSE3__
/* Four periods (4 n soft bits, 4 k pairs) as k shuffles: pairs 4c .. 4c + 3
 * come from the 8 soft bits at OFF[rate][c] of the four periods, MASK
 * picks their bytes (0x80: an erased bit, 0). */
static int8_t SHUF_OFF[6][8];
static uint8_t SHUF_MASK[6][8][16];
static pthread_once_t shuf_once = PTHREAD_ONCE_INIT;

static void init_shuf(void)
{
    for (int r = 1; r <= 5; r++) {
        const struct conv_punct* p = &CONV_PUNCT[r];
        int src[32][2]; /* soft bit index of X and Y of the 4 k pairs, or -1 */
        for (int j = 0; j < 4 * p->k; j++)
            src[j][0] = src[j][1] = -1;
        for (int b = 0; b < 4 * p->n; b++) {
            const int per = b / p->n, pos = b % p->n;
            src[per * p->k + p->idx[pos]][p->isy[pos]] = b;
        }
        for (int c = 0; c < p->k; c++) {
            int lo = 4 * p->n;
            for (int j = 4 * c; j < 4 * c + 4; j++)
                for (int e = 0; e < 2; e++)
                    if (src[j][e] >= 0 && src[j][e] < lo)
                        lo = src[j][e];
            SHUF_OFF[r][c] = (int8_t)lo;
            for (int j = 0; j < 4; j++)
                for (int e = 0; e < 2; e++) {
                    const int b = src[4 * c + j][e];
                    uint8_t* mk = &SHUF_MASK[r][c][4 * j + 2 * e];
                    mk[0] = b < 0 ? 0x80 : (uint8_t)(2 * (b - lo));
                    mk[1] = b < 0 ? 0x80 : (uint8_t)(2 * (b - lo) + 1);
                }
        }
    }
}
#endif

int conv_depuncture(struct conv_depunct* dp, const int16_t* soft, int n, int16_t (*pairs)[2])
{
    const struct conv_punct* p = dp->p;
    int i = 0, np = 0;
    if (dp->pos) {
        i = p->n - dp->pos < n ? p->n - dp->pos : n;
        np = depuncture_slow(dp, soft, i, pairs);
    }
    /* whole periods from here (cur is all zero at a period boundary) */
#ifdef __SSSE3__
    pthread_once(&shuf_once, init_shuf);
    const int r = (int)(p - CONV_PUNCT);
    /* the 16-byte loads may reach 8 soft bits past the four periods */
    for (; i + 4 * p->n + 8 <= n; i += 4 * p->n) {
        for (int c = 0; c < p->k; c++) {
            __m128i v, mk;
            memcpy(&v, soft + i + SHUF_OFF[r][c], sizeof(v));
            memcpy(&mk, SHUF_MASK[r][c], sizeof(mk));
            v = _mm_shuffle_epi8(v, mk);
            memcpy(pairs + np + 4 * c, &v, sizeof(v));
        }
        np += 4 * p->k;
    }
#endif
    /* the source of each X and Y of the period, or an erasure */
    int sx[8], sy[8];
    int16_t mx[8], my[8];
    for (int j = 0; j < p->k; j++)
        sx[j] = sy[j] = 0, mx[j] = my[j] = 0;
    for (int b = 0; b < p->n; b++) {
        if (p->isy[b])
            sy[p->idx[b]] = b, my[p->idx[b]] = -1;
        else
            sx[p->idx[b]] = b, mx[p->idx[b]] = -1;
    }
    for (; i + p->n <= n; i += p->n) {
        const int16_t* s = soft + i;
        for (int j = 0; j < p->k; j++) {
            pairs[np + j][0] = (int16_t)(s[sx[j]] & mx[j]);
            pairs[np + j][1] = (int16_t)(s[sy[j]] & my[j]);
        }
        np += p->k;
    }
    if (i < n)
        np += depuncture_slow(dp, soft + i, n - i, pairs + np);
    return np;
}

static int16_t SGX[32], SGY[32];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

/* Butterfly j of the step: states 2j and 2j+1 to j and j + 32, branch metric
 * a_j = SGX[j] * sx + SGY[j] * sy. */
static void init_tables(void)
{
    for (int j = 0; j < 32; j++) {
        const int r = 2 * j;
        SGX[j] = __builtin_parity(r & 0171) ? -1 : 1;
        SGY[j] = __builtin_parity(r & 0133) ? -1 : 1;
    }
}

/* Metrics grow by at most 2 x 127 a step and stay within ~1500 of each
 * other: subtracting state 0 every 16 steps keeps them far from overflow
 * (the decisions only depend on differences, so when does not matter). */
#define RENORM 16

#if defined(__AVX2__)

/* The 64 path metrics as int16 in four AVX2 vectors. The trellis step is a
 * perfect shuffle of the state bits; done naively each step costs a
 * deinterleave (pack) plus a lane fix (vpermq) of all four vectors, and port
 * 5 (the only shuffle port) is the bottleneck. Two layouts alternate
 * instead, so that only every other step needs the lane fix:
 *   Q (natural): vector m[k] holds states 16k .. 16k + 15 in order;
 *   P: vector V[c][a] holds the states with bit 3 = c and bit 5 = a, lane l
 *      (128-bit half) those with bit 4 = l, element e those with bits 2..0
 *      = e.
 * Step "Y" goes from Q to P: pairs (m0, m2) and (m1, m3) are deinterleaved
 * (packus of the even / odd int16) and the result's 64-bit quarters swapped
 * (vpermq), butterflies, outputs in P. Step "X" goes from P to Q: pairs
 * (V[0][a], V[1][a]) deinterleaved, no lane fix, outputs in Q. The
 * decisions come out in natural order (bit s for state s) after a vpermq
 * of the packed X decisions.
 *
 * Branch metrics: a_j = SGX[j] (sx + sy) when SGX[j] = SGY[j], else
 * SGX[j] (sx - sy); for this code, SGX[j] = SGY[j] exactly when bits 0 and
 * 4 of j are equal. With u = sx + sy, w = sx - sy, uw[t] = (u, w) and
 * wu[t] = (w, u): a 32-bit broadcast of either (a plain load) and a sign
 * (vpsignw) give the 16 metrics of a vector. */
static __m256i SIGN_Y[2], SIGN_X[2];
static pthread_once_t avx_once = PTHREAD_ONCE_INIT;

static void init_avx(void)
{
    pthread_once(&tables_once, init_tables);
    int16_t y[2][16], x[2][16];
    for (int b = 0; b < 2; b++)
        for (int l = 0; l < 2; l++)
            for (int e = 0; e < 8; e++) {
                /* Y: vector b, lane l, element e is butterfly 16 l + 8 b + e;
                 * X: vector a = b is butterfly 16 a + 8 l + e */
                y[b][8 * l + e] = SGX[16 * l + 8 * b + e];
                x[b][8 * l + e] = SGX[16 * b + 8 * l + e];
            }
    for (int b = 0; b < 2; b++) {
        memcpy(&SIGN_Y[b], y[b], sizeof(SIGN_Y[b]));
        memcpy(&SIGN_X[b], x[b], sizeof(SIGN_X[b]));
    }
}

static inline __m256i bcast32(const uint32_t* p) { return _mm256_set1_epi32((int)*p); }

/* uw[t], wu[t] of n steps */
static void branch_metrics(const int16_t (*pairs)[2], int n, uint32_t* uw, uint32_t* wu)
{
    int t = 0;
    for (; t + 8 <= n; t += 8) {
        const __m256i p = _mm256_loadu_si256((const __m256i*)(pairs + t)); /* (sx, sy) */
        const __m256i q = _mm256_or_si256(_mm256_slli_epi32(p, 16), _mm256_srli_epi32(p, 16));
        const __m256i u = _mm256_add_epi16(p, q), d = _mm256_sub_epi16(p, q); /* (u, u), (w, -w) */
        _mm256_storeu_si256((__m256i*)(uw + t), _mm256_blend_epi16(u, _mm256_sub_epi16(q, p), 0xAA));
        _mm256_storeu_si256((__m256i*)(wu + t), _mm256_blend_epi16(d, u, 0xAA));
    }
    for (; t < n; t++) {
        const uint16_t u = (uint16_t)(pairs[t][0] + pairs[t][1]), w = (uint16_t)(pairs[t][0] - pairs[t][1]);
        uw[t] = u | (uint32_t)w << 16;
        wu[t] = w | (uint32_t)u << 16;
    }
}

/* even and odd int16 of (a, b), each per 128-bit lane: [a evens, b evens] */
#define DEINTERLEAVE(a, b, ev, od)                                                                 \
    do {                                                                                           \
        ev = _mm256_packus_epi32(_mm256_and_si256(a, lo16), _mm256_and_si256(b, lo16));            \
        od = _mm256_packus_epi32(_mm256_srli_epi32(a, 16), _mm256_srli_epi32(b, 16));              \
    } while (0)

/* butterflies of 16 pairs: new metrics (j and j + 32) and decisions */
#define BUTTERFLY(ev, od, a, nlo, nhi, dlo, dhi)                                                   \
    do {                                                                                           \
        const __m256i p0_ = _mm256_add_epi16(ev, a), p1_ = _mm256_sub_epi16(od, a);                \
        const __m256i q0_ = _mm256_sub_epi16(ev, a), q1_ = _mm256_add_epi16(od, a);                \
        nlo = _mm256_max_epi16(p0_, p1_);                                                          \
        nhi = _mm256_max_epi16(q0_, q1_);                                                          \
        dlo = _mm256_cmpgt_epi16(p1_, p0_);                                                        \
        dhi = _mm256_cmpgt_epi16(q1_, q0_);                                                        \
    } while (0)

static inline uint64_t dec_word(__m256i lo, __m256i hi)
{
    return (uint64_t)(uint32_t)_mm256_movemask_epi8(lo) |
           (uint64_t)(uint32_t)_mm256_movemask_epi8(hi) << 32;
}

/* Y step (Q -> P) */
#define STEP_Y(uwp, wup, decp)                                                                          \
    do {                                                                                           \
        __m256i e0, o0, e1, o1, d0, d1, d2, d3;                                                    \
        DEINTERLEAVE(m0, m2, e0, o0);                                                              \
        DEINTERLEAVE(m1, m3, e1, o1);                                                              \
        e0 = _mm256_permute4x64_epi64(e0, 0xD8);                                                   \
        o0 = _mm256_permute4x64_epi64(o0, 0xD8);                                                   \
        e1 = _mm256_permute4x64_epi64(e1, 0xD8);                                                   \
        o1 = _mm256_permute4x64_epi64(o1, 0xD8);                                                   \
        const __m256i b_ = _mm256_blend_epi32(bcast32(uwp), bcast32(wup), 0xF0);                   \
        const __m256i a0 = _mm256_sign_epi16(b_, SIGN_Y[0]), a1 = _mm256_sign_epi16(b_, SIGN_Y[1]); \
        BUTTERFLY(e0, o0, a0, v00, v01, d0, d1);                                                   \
        BUTTERFLY(e1, o1, a1, v10, v11, d2, d3);                                                   \
        *(decp) = dec_word(_mm256_packs_epi16(d0, d2), _mm256_packs_epi16(d1, d3));                \
    } while (0)

/* X step (P -> Q) */
#define STEP_X(uwp, wup, decp)                                                                          \
    do {                                                                                           \
        __m256i e0, o0, e1, o1, d0, d1, d2, d3;                                                    \
        DEINTERLEAVE(v00, v10, e0, o0);                                                            \
        DEINTERLEAVE(v01, v11, e1, o1);                                                            \
        const __m256i a0 = _mm256_sign_epi16(bcast32(uwp), SIGN_X[0]);                             \
        const __m256i a1 = _mm256_sign_epi16(bcast32(wup), SIGN_X[1]);                             \
        BUTTERFLY(e0, o0, a0, m0, m2, d0, d1);                                                     \
        BUTTERFLY(e1, o1, a1, m1, m3, d2, d3);                                                     \
        *(decp) = dec_word(_mm256_permute4x64_epi64(_mm256_packs_epi16(d0, d2), 0xD8),             \
                           _mm256_permute4x64_epi64(_mm256_packs_epi16(d1, d3), 0xD8));            \
    } while (0)

#define BM_BLOCK 512

/* n trellis steps from the natural-order metrics m (updated); dec[t] gets
 * the decisions of step t, bit s set when the survivor into state s comes
 * from the odd state. */
/* (no SLP: GCC would merge the decision words of the two steps in a vector
 * register, with vpinsrq on the shuffle port that is the bottleneck) */
__attribute__((optimize("no-tree-slp-vectorize"))) static void
trellis(int16_t* m, const int16_t (*pairs)[2], int n, uint64_t* dec)
{
    pthread_once(&avx_once, init_avx);
    const __m256i lo16 = _mm256_set1_epi32(0xFFFF);
    __m256i m0 = _mm256_loadu_si256((const __m256i*)m), m1 = _mm256_loadu_si256((const __m256i*)(m + 16));
    __m256i m2 = _mm256_loadu_si256((const __m256i*)(m + 32)), m3 = _mm256_loadu_si256((const __m256i*)(m + 48));
    __m256i v00, v01, v10, v11;
    uint32_t uw[BM_BLOCK], wu[BM_BLOCK];
    for (int t0 = 0; t0 < n; t0 += BM_BLOCK) {
        const int nb = n - t0 < BM_BLOCK ? n - t0 : BM_BLOCK;
        branch_metrics(pairs + t0, nb, uw, wu);
        uint64_t* d = dec + t0;
        int t = 0;
        for (; t + 2 <= nb; t += 2) {
            STEP_Y(uw + t, wu + t, d + t);
            STEP_X(uw + t + 1, wu + t + 1, d + t + 1);
            if ((t & (RENORM - 1)) == RENORM - 2) {
                const __m256i ref = _mm256_broadcastw_epi16(_mm256_castsi256_si128(m0));
                m0 = _mm256_sub_epi16(m0, ref);
                m1 = _mm256_sub_epi16(m1, ref);
                m2 = _mm256_sub_epi16(m2, ref);
                m3 = _mm256_sub_epi16(m3, ref);
            }
        }
        if (t < nb) {
            /* odd step: Y, then P back to natural order */
            STEP_Y(uw + t, wu + t, d + t);
            m0 = _mm256_permute2x128_si256(v00, v10, 0x20);
            m1 = _mm256_permute2x128_si256(v00, v10, 0x31);
            m2 = _mm256_permute2x128_si256(v01, v11, 0x20);
            m3 = _mm256_permute2x128_si256(v01, v11, 0x31);
        }
        const __m256i ref = _mm256_broadcastw_epi16(_mm256_castsi256_si128(m0));
        m0 = _mm256_sub_epi16(m0, ref);
        m1 = _mm256_sub_epi16(m1, ref);
        m2 = _mm256_sub_epi16(m2, ref);
        m3 = _mm256_sub_epi16(m3, ref);
    }
    _mm256_storeu_si256((__m256i*)m, m0);
    _mm256_storeu_si256((__m256i*)(m + 16), m1);
    _mm256_storeu_si256((__m256i*)(m + 32), m2);
    _mm256_storeu_si256((__m256i*)(m + 48), m3);
}

#else /* portable: GCC vector extensions */

typedef int16_t v16s __attribute__((vector_size(32)));
typedef int8_t v16b __attribute__((vector_size(16)));

static v16s SGXV[2], SGYV[2]; /* SGX, SGY of butterflies 0..15 and 16..31 */
static pthread_once_t vec_once = PTHREAD_ONCE_INIT;

static void init_vec(void)
{
    pthread_once(&tables_once, init_tables);
    for (int j = 0; j < 32; j++) {
        SGXV[j / 16][j % 16] = SGX[j];
        SGYV[j / 16][j % 16] = SGY[j];
    }
}

/* The 64 path metrics in four vectors of 16 int16: state s is lane s % 16
 * of vector s / 16. */
struct metrics {
    v16s v[4];
};

#define EVENS 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30
#define ODDS 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31

static inline v16s vmax(v16s a, v16s b, v16s gt) { return (b & gt) | (a & ~gt); }

/* 16 decisions (bytes 0 / -1) to 16 bits */
static inline uint64_t mask16(v16b b)
{
#ifdef __SSE2__
    __m128i x;
    memcpy(&x, &b, sizeof(x));
    return (uint64_t)(uint16_t)_mm_movemask_epi8(x);
#else
    uint64_t r = 0;
    for (int i = 0; i < 16; i++)
        r |= (uint64_t)(b[i] & 1) << i;
    return r;
#endif
}

static inline uint64_t acs(struct metrics* m, int16_t sx, int16_t sy)
{
    uint64_t dec = 0;
    v16s nlo[2], nhi[2];
    for (int h = 0; h < 2; h++) {
        const v16s e = __builtin_shufflevector(m->v[2 * h], m->v[2 * h + 1], EVENS);
        const v16s o = __builtin_shufflevector(m->v[2 * h], m->v[2 * h + 1], ODDS);
        const v16s a = SGXV[h] * sx + SGYV[h] * sy;
        const v16s p0 = e + a, p1 = o - a, q0 = e - a, q1 = o + a;
        const v16s dp = p1 > p0, dq = q1 > q0;
        nlo[h] = vmax(p0, p1, dp);
        nhi[h] = vmax(q0, q1, dq);
        dec |= mask16(__builtin_convertvector(dp, v16b)) << (16 * h);
        dec |= mask16(__builtin_convertvector(dq, v16b)) << (32 + 16 * h);
    }
    m->v[0] = nlo[0];
    m->v[1] = nlo[1];
    m->v[2] = nhi[0];
    m->v[3] = nhi[1];
    return dec;
}

static void trellis(int16_t* m, const int16_t (*pairs)[2], int n, uint64_t* dec)
{
    pthread_once(&vec_once, init_vec);
    struct metrics mv;
    memcpy(&mv, m, sizeof(mv));
    for (int t = 0; t < n; t++) {
        dec[t] = acs(&mv, pairs[t][0], pairs[t][1]);
        if ((t & (RENORM - 1)) == RENORM - 1 || t == n - 1) {
            const int16_t ref = mv.v[0][0];
            for (int k = 0; k < 4; k++)
                mv.v[k] -= ref;
        }
    }
    memcpy(m, &mv, sizeof(mv));
}

#endif

static inline int best_state(const int16_t* m)
{
    int b = 0;
    for (int s = 1; s < 64; s++)
        if (m[s] > m[b])
            b = s;
    return b;
}

/* Traceback: the survivor of state s before a step is ((s & 31) << 1) |
 * (bit s of the step's decisions). The state is kept unmasked (only its low
 * 6 bits mean anything, and a shift count or bit index is taken modulo 64):
 * s' = 2 s + bit, two dependent instructions with bt / adc, one cycle each
 * (the traceback is a chain of dependent steps). */
static inline uint64_t prev_state(uint64_t s, uint64_t d)
{
#if defined(__x86_64__) && defined(__GNUC__)
    __asm__("bt %[s], %[d]\n\tadc %[s], %[s]" : [s] "+r"(s) : [d] "r"(d) : "cc");
    return s;
#else
    return 2 * s + ((d >> (s & 63)) & 1);
#endif
}

/* Trace back through dec[hi - 1] down to dec[lo] from state s. */
static inline uint64_t trace(const uint64_t* dec, int lo, int hi, uint64_t s)
{
    for (int t = hi - 1; t >= lo; t--)
        s = prev_state(s, dec[t]);
    return s;
}

/* The same, writing the decided bit of each step to bits[t - lo]. */
static inline uint64_t trace_out(const uint64_t* dec, int lo, int hi, uint64_t s, uint8_t* bits)
{
    for (int t = hi - 1; t >= lo; t--) {
        bits[t - lo] = (uint8_t)((s >> 5) & 1);
        s = prev_state(s, dec[t]);
    }
    return s;
}

void conv_viterbi_block(const int16_t (*pairs)[2], int n, uint8_t* bits, uint64_t* dec)
{
    int16_t m[64] = { 0 };
    trellis(m, pairs, n, dec);
    trace_out(dec, 0, n, (uint64_t)best_state(m), bits);
}

double conv_mismatch(const int16_t* soft, int n_soft, int rate, int phase)
{
    struct conv_depunct dp = { .p = &CONV_PUNCT[rate], .pos = phase };
    int16_t (*pairs)[2] = malloc(sizeof(*pairs) * (size_t)(n_soft + 8));
    uint8_t* bits = malloc((size_t)n_soft + 8);
    uint64_t* dec = malloc(sizeof(*dec) * (size_t)(n_soft + 8));
    double r = 1.0;
    const int np = pairs && bits && dec ? conv_depuncture(&dp, soft, n_soft, pairs) : 0;
    if (np >= 256) {
        conv_viterbi_block((const int16_t (*)[2])pairs, np, bits, dec);
        int errors = 0, count = 0;
        unsigned reg = 0;
        for (int t = 0; t < np; t++) {
            reg = (reg >> 1) | ((unsigned)bits[t] << 6);
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
        r = count ? (double)errors / count : 1.0;
    }
    free(pairs);
    free(bits);
    free(dec);
    return r;
}

int conv_viterbi_init(struct conv_viterbi* v)
{
    pthread_once(&tables_once, init_tables);
    memset(v, 0, sizeof(*v));
    v->dec = malloc(sizeof(*v->dec) * CONV_RING);
    v->out = malloc(CONV_CHUNK);
    return v->dec && v->out ? 0 : -1;
}

void conv_viterbi_free(struct conv_viterbi* v)
{
    free(v->dec);
    free(v->out);
}

void conv_viterbi_reset(struct conv_viterbi* v)
{
    memset(v->m, 0, sizeof(v->m));
    v->head = v->pending = 0;
}

void conv_viterbi_decode(struct conv_viterbi* v, const int16_t (*pairs)[2], int n,
                         void (*bits_cb)(void* ctx, const uint8_t* bits, int n), void* ctx,
                         const int* run)
{
    int t = 0;
    while (t < n && *run) {
        /* steps up to the next traceback or the end of the ring */
        int k = n - t;
        if (k > CONV_RING - v->pending)
            k = CONV_RING - v->pending;
        if (k > CONV_RING - v->head)
            k = CONV_RING - v->head;
        trellis(v->m, pairs + t, k, v->dec + v->head);
        t += k;
        v->pending += k;
        v->head += k;
        if (v->head == CONV_RING)
            v->head = 0;
        if (v->pending < CONV_RING)
            continue;
        /* trace back CONV_TB steps, then decide the CONV_CHUNK oldest ones;
         * the ring holds exactly CONV_RING steps, the oldest at head */
        const uint64_t* dec = v->dec;
        const int h = v->head;
        uint64_t s = (uint64_t)best_state(v->m);
        /* steps in ring order: [h, CONV_RING) then [0, h) */
        if (h >= CONV_TB) {
            s = trace(dec, h - CONV_TB, h, s);
            /* CONV_CHUNK steps before h - CONV_TB, wrapping */
            const int a = h - CONV_TB; /* steps of the chunk in [0, a) */
            s = trace_out(dec, 0, a, s, v->out + CONV_CHUNK - a);
            trace_out(dec, CONV_RING - (CONV_CHUNK - a), CONV_RING, s, v->out);
        } else {
            s = trace(dec, 0, h, s);
            s = trace(dec, CONV_RING - (CONV_TB - h), CONV_RING, s);
            /* the chunk: [h, CONV_RING - (CONV_TB - h)) */
            trace_out(dec, h, h + CONV_CHUNK, s, v->out);
        }
        v->pending -= CONV_CHUNK;
        bits_cb(ctx, v->out, CONV_CHUNK);
    }
}
