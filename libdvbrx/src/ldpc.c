/*
 * DVB-S2 / S2X LDPC decoder, see ldpc.h.
 *
 * C port of the layered offset min-sum decoder by Ahmet Inan and Ron
 * Economos used in gr-dvbs2rx (GPL-3.0-or-later). Check nodes are processed
 * layer by layer; each SIMD byte lane carries a different frame. Check to
 * variable messages are stored clamped to [-32, 31].
 */
#include "dvbrx/ldpc.h"

#include <stdlib.h>
#include <string.h>

#if defined(__AVX512BW__) || defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

struct ldpc_table {
    int M, N, K, links_max_cn, links_total, deg_max;
    const uint16_t *deg, *len, *pos;
};
struct ldpc_map {
    const char* rate;
    const struct ldpc_table* table;
};
#include "ldpc_tables.h"

/* ---- 8-bit SIMD operations ------------------------------------------- */

#if defined(__AVX512BW__)
#define LANES 64
typedef __m512i vec;
static inline vec v_zero(void) { return _mm512_setzero_si512(); }
static inline vec v_dup(int8_t c) { return _mm512_set1_epi8(c); }
static inline vec v_qadd(vec a, vec b) { return _mm512_adds_epi8(a, b); }
static inline vec v_qsub(vec a, vec b) { return _mm512_subs_epi8(a, b); }
static inline vec v_min(vec a, vec b) { return _mm512_min_epi8(a, b); }
static inline vec v_max(vec a, vec b) { return _mm512_max_epi8(a, b); }
static inline vec v_xor(vec a, vec b) { return _mm512_xor_si512(a, b); }
static inline vec v_or(vec a, vec b) { return _mm512_or_si512(a, b); }
/* |a| saturated to 127, minus 1 (unsigned saturating) */
static inline vec v_mag(vec a)
{
    vec m = _mm512_min_epu8(_mm512_abs_epi8(a), v_dup(127));
    return _mm512_subs_epu8(m, v_dup(1));
}
/* a if b > 0, -a if b < 0, 0 if b == 0 */
static inline vec v_sign(vec a, vec b)
{
    vec r = _mm512_mask_sub_epi8(a, _mm512_movepi8_mask(b), v_zero(), a);
    return _mm512_maskz_mov_epi8(_mm512_test_epi8_mask(b, b), r);
}
/* a == b ? c : b */
static inline vec v_other(vec a, vec b, vec c)
{
    return _mm512_mask_blend_epi8(_mm512_cmpeq_epi8_mask(a, b), b, c);
}
static inline int v_all_positive(vec a)
{
    return _mm512_cmpgt_epi8_mask(a, v_zero()) == ~0ULL;
}
/* (a << 1) | (b < 0) in each byte */
static inline vec v_shift_in_sign(vec a, vec b)
{
    return _mm512_sub_epi8(_mm512_add_epi8(a, a), _mm512_movm_epi8(_mm512_movepi8_mask(b)));
}
#elif defined(__AVX2__)
#define LANES 32
typedef __m256i vec;
static inline vec v_zero(void) { return _mm256_setzero_si256(); }
static inline vec v_dup(int8_t c) { return _mm256_set1_epi8(c); }
static inline vec v_qadd(vec a, vec b) { return _mm256_adds_epi8(a, b); }
static inline vec v_qsub(vec a, vec b) { return _mm256_subs_epi8(a, b); }
static inline vec v_min(vec a, vec b) { return _mm256_min_epi8(a, b); }
static inline vec v_max(vec a, vec b) { return _mm256_max_epi8(a, b); }
static inline vec v_xor(vec a, vec b) { return _mm256_xor_si256(a, b); }
static inline vec v_or(vec a, vec b) { return _mm256_or_si256(a, b); }
static inline vec v_mag(vec a)
{
    vec m = _mm256_min_epu8(_mm256_abs_epi8(a), v_dup(127));
    return _mm256_subs_epu8(m, v_dup(1));
}
static inline vec v_sign(vec a, vec b) { return _mm256_sign_epi8(a, b); }
static inline vec v_other(vec a, vec b, vec c)
{
    return _mm256_blendv_epi8(b, c, _mm256_cmpeq_epi8(a, b));
}
static inline int v_all_positive(vec a)
{
    return _mm256_movemask_epi8(_mm256_cmpgt_epi8(a, v_zero())) == -1;
}
static inline vec v_shift_in_sign(vec a, vec b)
{
    return _mm256_sub_epi8(_mm256_add_epi8(a, a), _mm256_cmpgt_epi8(v_zero(), b));
}
#else
#define LANES 16
typedef struct {
    int8_t v[LANES];
} vec;
static inline int8_t sat(int x) { return (int8_t)(x > 127 ? 127 : x < -128 ? -128 : x); }
#define VLOOP(expr)                    \
    vec r;                             \
    for (int i = 0; i < LANES; i++)    \
        r.v[i] = (expr);               \
    return r
static inline vec v_zero(void) { VLOOP(0); }
static inline vec v_dup(int8_t c) { VLOOP(c); }
static inline vec v_qadd(vec a, vec b) { VLOOP(sat(a.v[i] + b.v[i])); }
static inline vec v_qsub(vec a, vec b) { VLOOP(sat(a.v[i] - b.v[i])); }
static inline vec v_min(vec a, vec b) { VLOOP(a.v[i] < b.v[i] ? a.v[i] : b.v[i]); }
static inline vec v_max(vec a, vec b) { VLOOP(a.v[i] > b.v[i] ? a.v[i] : b.v[i]); }
static inline vec v_xor(vec a, vec b) { VLOOP(a.v[i] ^ b.v[i]); }
static inline vec v_or(vec a, vec b) { VLOOP(a.v[i] | b.v[i]); }
static inline vec v_mag(vec a)
{
    VLOOP((int8_t)((a.v[i] < 0 ? (a.v[i] == -128 ? 127 : -a.v[i]) : a.v[i]) > 0
                       ? (a.v[i] < 0 ? (a.v[i] == -128 ? 127 : -a.v[i]) : a.v[i]) - 1
                       : 0));
}
static inline vec v_sign(vec a, vec b)
{
    VLOOP((int8_t)(b.v[i] > 0 ? a.v[i] : b.v[i] < 0 ? -a.v[i] : 0));
}
static inline vec v_other(vec a, vec b, vec c) { VLOOP(a.v[i] == b.v[i] ? c.v[i] : b.v[i]); }
static inline int v_all_positive(vec a)
{
    for (int i = 0; i < LANES; i++)
        if (a.v[i] <= 0)
            return 0;
    return 1;
}
static inline vec v_shift_in_sign(vec a, vec b) { VLOOP((int8_t)((uint8_t)a.v[i] << 1 | (b.v[i] < 0))); }
#endif

/* ---- decoder ----------------------------------------------------------- */

struct drx_ldpc {
    int N, K, M, R, q, CNL, LT;
    int kldpc;
    uint16_t* pos; /* R * CNL variable node indexes, in layer order */
    uint8_t* cnc;  /* links to information bits, per check node */
    vec* bnl;      /* LT check to variable messages */
    vec* data;     /* N variable nodes: K information then R parity */
    vec* pty;      /* R parity nodes in layer order */
};

int drx_ldpc_lanes(void) { return LANES; }

static const struct ldpc_table* find_table(int short_frame, const char* rate)
{
    const struct ldpc_map* map = short_frame ? ldpc_map_short : ldpc_map_normal;
    const int n = short_frame ? (int)(sizeof(ldpc_map_short) / sizeof(*ldpc_map_short))
                              : (int)(sizeof(ldpc_map_normal) / sizeof(*ldpc_map_normal));
    for (int i = 0; i < n; i++)
        if (strcmp(map[i].rate, rate) == 0)
            return map[i].table;
    return NULL;
}

drx_ldpc* drx_ldpc_new(const drx_fec_params* p, const char* rate)
{
    const struct ldpc_table* t = find_table(p->short_frame, rate);
    if (!t)
        return NULL;
    drx_ldpc* d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->N = t->N;
    d->K = t->K;
    d->M = t->M;
    d->R = t->N - t->K;
    d->q = d->R / d->M;
    d->CNL = t->links_max_cn - 2;
    d->LT = t->links_total;
    d->kldpc = p->kldpc;

    uint16_t* pos = calloc((size_t)d->R * d->CNL, sizeof(uint16_t));
    d->pos = malloc(sizeof(uint16_t) * d->R * d->CNL);
    d->cnc = calloc(d->R, 1);
    d->bnl = aligned_alloc(sizeof(vec), sizeof(vec) * d->LT);
    d->data = aligned_alloc(sizeof(vec), sizeof(vec) * d->N);
    d->pty = aligned_alloc(sizeof(vec), sizeof(vec) * d->R);
    if (!pos || !d->pos || !d->cnc || !d->bnl || !d->data || !d->pty) {
        free(pos);
        drx_ldpc_free(d);
        return NULL;
    }

    /* Walk the information bits: each table row gives the parity
     * accumulators of the first bit of a 360-bit group; the following bits
     * of the group use addresses shifted by q (mod R). */
    const uint16_t* row = t->pos;
    int j = 0;
    for (int g = 0; t->len[g]; g++) {
        const int deg = t->deg[g];
        for (int r = 0; r < t->len[g]; r++, row += deg) {
            for (int m = 0; m < d->M; m++, j++) {
                for (int n = 0; n < deg; n++) {
                    int acc = (row[n] + m * d->q) % d->R;
                    pos[d->CNL * acc + d->cnc[acc]++] = (uint16_t)j;
                }
            }
        }
    }
    /* Layer order: check node q*j + i becomes M*i + j. */
    for (int i = 0; i < d->q; i++)
        for (int jj = 0; jj < d->M; jj++)
            memcpy(d->pos + d->CNL * (d->M * i + jj), pos + d->CNL * (d->q * jj + i),
                   sizeof(uint16_t) * d->CNL);
    free(pos);
    return d;
}

void drx_ldpc_free(drx_ldpc* d)
{
    if (!d)
        return;
    free(d->pos);
    free(d->cnc);
    free(d->bnl);
    free(d->data);
    free(d->pty);
    free(d);
}

/* Index of the parity node feeding check node (i, j) from the previous
 * check node through the accumulator, or -1 for the very first one. */
static inline int prev_parity(const drx_ldpc* d, int i, int j)
{
    if (i)
        return d->M * (i - 1) + j;
    if (j)
        return j + (d->q - 1) * d->M - 1;
    return -1;
}

static int bad(const drx_ldpc* d)
{
    const vec one = v_dup(1);
    for (int i = 0; i < d->q; i++) {
        const int cnt = d->cnc[i];
        for (int j = 0; j < d->M; j++) {
            const int cn = d->M * i + j;
            vec cnv = v_sign(one, d->pty[cn]);
            const int pp = prev_parity(d, i, j);
            if (pp >= 0)
                cnv = v_sign(cnv, d->pty[pp]);
            const uint16_t* p = d->pos + d->CNL * cn;
            for (int c = 0; c < cnt; c++)
                cnv = v_sign(cnv, d->data[p[c]]);
            if (!v_all_positive(cnv))
                return 1;
        }
    }
    return 0;
}

#define MAX_DEG 32 /* check node degree, links_max_cn <= 30 in the tables */

/*
 * Layered offset min-sum (offset 1), one check node: the first pass forms
 * the extrinsic inputs (posterior minus the previous check to variable
 * message) on the cnt information links p[] and the parity links pc and
 * pp (NULL for the very first check node), their two smallest magnitudes
 * and their sign product; the second pass recomputes each magnitude
 * (storing it was slower: stores are as much a limit as the vector ALU
 * ports) and writes the new posteriors and the new messages, clamped to
 * [-32, 31]. Same arithmetic as before, in fewer passes over the links.
 *
 * bl: the node's messages; returns the next node's. first: no previous
 * messages (all zero), so bnl is neither cleared nor read.
 */
static inline __attribute__((always_inline)) vec* check_node(drx_ldpc* d, vec* bl, const uint16_t* p,
                                                              vec* pc, vec* pp, int cnt, int first)
{
    const vec lo = v_dup(-32), hi = v_dup(31), c127 = v_dup(127);
    vec* data = d->data;
    vec inp[MAX_DEG];
    vec min0 = c127, min1 = c127, signs = v_zero();
#define CN_IN(c, v)                                                \
    do {                                                           \
        const vec x = first ? (v) : v_qsub((v), bl[c]);            \
        const vec m = v_mag(x);                                    \
        inp[c] = x;                                                \
        min1 = v_min(min1, v_max(min0, m));                        \
        min0 = v_min(min0, m);                                     \
        signs = v_xor(signs, x);                                   \
    } while (0)
#define CN_OUT(c, dst)                                                                    \
    do {                                                                                  \
        const vec x = inp[c];                                                             \
        const vec o = v_sign(v_other(v_mag(x), min0, min1), v_or(v_xor(signs, x), c127)); \
        (dst) = v_qadd(x, o);                                                             \
        bl[c] = v_min(v_max(o, lo), hi);                                                  \
    } while (0)
    for (int c = 0; c < cnt; c++)
        CN_IN(c, data[p[c]]);
    CN_IN(cnt, *pc);
    if (pp)
        CN_IN(cnt + 1, *pp);
    for (int c = 0; c < cnt; c++)
        CN_OUT(c, data[p[c]]);
    CN_OUT(cnt, *pc);
    if (pp)
        CN_OUT(cnt + 1, *pp);
#undef CN_IN
#undef CN_OUT
    return bl + cnt + 1 + (pp != NULL);
}

/* Layer i: check nodes M i .. M i + M - 1, all with cnt information links. */
static inline __attribute__((always_inline)) vec* layer(drx_ldpc* d, vec* bl, int i, int cnt,
                                                         int first)
{
    int j = 0;
    if (!i) { /* check node 0 has no previous parity node */
        bl = check_node(d, bl, d->pos, d->pty, NULL, cnt, first);
        j = 1;
    }
    for (; j < d->M; j++) {
        const int cn = d->M * i + j;
        bl = check_node(d, bl, d->pos + d->CNL * cn, d->pty + cn, d->pty + prev_parity(d, i, j),
                        cnt, first);
    }
    return bl;
}

static void update(drx_ldpc* d)
{
    vec* bl = d->bnl;
    for (int i = 0; i < d->q; i++)
        bl = layer(d, bl, i, d->cnc[i], 0);
}

/* first iteration: all messages are zero */
static void update_first(drx_ldpc* d)
{
    vec* bl = d->bnl;
    for (int i = 0; i < d->q; i++)
        bl = layer(d, bl, i, d->cnc[i], 1);
}

/* ---- frame <-> lane reordering ------------------------------------------ */

/* Frames come in as one byte array each and the decoder works on one byte
 * per frame in each vector, so the input and the hard decisions are byte
 * matrix transposes; done 16 x 16 with SSE2 unpacks rather than byte by
 * byte (that was a quarter of the decoding time at 4 iterations). */
#if defined(__SSE2__)
/* Transposes x[0..15] (rows of 16 bytes); column c ends up in x[REV4[c]]. */
static const uint8_t REV4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
static inline void tr16(__m128i* x)
{
    __m128i y[16];
    for (int i = 0; i < 8; i++) {
        y[i] = _mm_unpacklo_epi8(x[2 * i], x[2 * i + 1]);
        y[i + 8] = _mm_unpackhi_epi8(x[2 * i], x[2 * i + 1]);
    }
    for (int i = 0; i < 8; i++) {
        x[i] = _mm_unpacklo_epi16(y[2 * i], y[2 * i + 1]);
        x[i + 8] = _mm_unpackhi_epi16(y[2 * i], y[2 * i + 1]);
    }
    for (int i = 0; i < 8; i++) {
        y[i] = _mm_unpacklo_epi32(x[2 * i], x[2 * i + 1]);
        y[i + 8] = _mm_unpackhi_epi32(x[2 * i], x[2 * i + 1]);
    }
    for (int i = 0; i < 8; i++) {
        x[i] = _mm_unpacklo_epi64(y[2 * i], y[2 * i + 1]);
        x[i + 8] = _mm_unpackhi_epi64(y[2 * i], y[2 * i + 1]);
    }
}
#endif

/* data[j][f] = llr[f][j]; lanes without a frame get 127 (all-zero
 * codeword, which satisfies every check). */
static void load_frames(drx_ldpc* d, const int8_t* const* llr, int nframes)
{
    int8_t* data = (int8_t*)d->data;
    int j0 = 0;
#if defined(__SSE2__)
    for (; j0 + 16 <= d->N; j0 += 16) {
        for (int f0 = 0; f0 < LANES; f0 += 16) {
            __m128i x[16];
            for (int r = 0; r < 16; r++)
                x[r] = f0 + r < nframes ? _mm_loadu_si128((const __m128i*)(llr[f0 + r] + j0))
                                        : _mm_set1_epi8(127);
            tr16(x);
            for (int c = 0; c < 16; c++)
                _mm_storeu_si128((__m128i*)(data + (size_t)(j0 + c) * LANES + f0), x[REV4[c]]);
        }
    }
#endif
    for (int j = j0; j < d->N; j++) {
        int8_t* v = data + (size_t)j * LANES;
        for (int f = 0; f < nframes; f++)
            v[f] = llr[f][j];
        for (int f = nframes; f < LANES; f++)
            v[f] = 127;
    }
}

/* Hard decisions on the first kldpc bits (all information bits for
 * DVB-S2; kldpc == K), packed MSB first: eight variable nodes give one
 * output byte per lane, then 16 such bytes are transposed per frame. */
static void store_frames(const drx_ldpc* d, uint8_t* const* out, int nframes)
{
    const int nbytes = (d->kldpc + 7) / 8;
    for (int b0 = 0; b0 < nbytes; b0 += 16) {
        const int nb = nbytes - b0 < 16 ? nbytes - b0 : 16;
        vec bytes[16];
        for (int b = 0; b < nb; b++) {
            const vec* x = d->data + 8 * (b0 + b);
            vec acc = v_zero();
            for (int k = 0; k < 8; k++)
                acc = v_shift_in_sign(acc, x[k]);
            bytes[b] = acc;
        }
        const uint8_t* t = (const uint8_t*)bytes;
#if defined(__SSE2__)
        if (nb == 16) {
            for (int f0 = 0; f0 < nframes; f0 += 16) {
                __m128i x[16];
                for (int r = 0; r < 16; r++)
                    x[r] = _mm_loadu_si128((const __m128i*)(t + r * LANES + f0));
                tr16(x);
                for (int c = 0; c < 16 && f0 + c < nframes; c++)
                    _mm_storeu_si128((__m128i*)(out[f0 + c] + b0), x[REV4[c]]);
            }
            continue;
        }
#endif
        for (int f = 0; f < nframes; f++)
            for (int b = 0; b < nb; b++)
                out[f][b0 + b] = t[b * LANES + f];
    }
}

int drx_ldpc_decode(drx_ldpc* d,
                    const int8_t* const* llr,
                    uint8_t* const* out,
                    int nframes,
                    int max_iter)
{
    if (nframes > LANES)
        return -1;

    load_frames(d, llr, nframes);
    for (int i = 0; i < d->q; i++)
        for (int j = 0; j < d->M; j++)
            d->pty[d->M * i + j] = d->data[d->K + d->q * j + i];
    int it = 0, ok;
    while (!(ok = !bad(d)) && it < max_iter) {
        if (it)
            update(d);
        else
            update_first(d);
        it++;
    }

    store_frames(d, out, nframes);
    return ok ? it : -1;
}
