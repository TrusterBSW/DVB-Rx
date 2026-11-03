/*
 * DVB-S2 BCH decoder (EN 302 307-1, Section 5.3.1).
 *
 * The generator polynomial is built as the product of the minimal
 * polynomials of alpha^1, alpha^3, ..., alpha^(2t-1) over GF(2^m), m = 16
 * for normal frames and 14 for short ones. Decoding first divides the
 * received word by g(x) with a byte-wise table; only when the remainder is
 * non-zero does it compute syndromes, run Berlekamp-Massey and a Chien
 * search.
 */
#include "dvbrx/fec.h"

#include <stdlib.h>
#include <string.h>

struct fec_row {
    const char* rate;
    int kbch, nbch, t;
};
#include "fec_table.h"

#define MAX_T 12
#define NW 4 /* 64-bit words of the left-aligned remainder register */

int drx_fec_lookup(int short_frame, const char* rate, drx_fec_params* p)
{
    const struct fec_row* rows = short_frame ? fec_short : fec_normal;
    const int n = short_frame ? (int)(sizeof(fec_short) / sizeof(*fec_short))
                              : (int)(sizeof(fec_normal) / sizeof(*fec_normal));
    for (int i = 0; i < n; i++) {
        if (strcmp(rows[i].rate, rate) == 0) {
            p->short_frame = short_frame;
            p->nldpc = short_frame ? 16200 : 64800;
            p->kldpc = rows[i].nbch;
            p->kbch = rows[i].kbch;
            p->t = rows[i].t;
            return 0;
        }
    }
    return -1;
}

struct drx_bch {
    int m, q;   /* field GF(2^m), q = 2^m - 1 */
    int t, nbch;
    int W;      /* degree of g(x) = number of parity bits */
    uint16_t* exp; /* 2q entries */
    int* log;
    /* table[s][v] = (v(x) x^(W + 8 s)) mod g(x), left-aligned in NW words:
     * the remainder is computed 4 bytes at a time (slicing by 4), cutting
     * the chain of dependent table lookups by 4. */
    uint64_t table[4][256][NW];
};

static inline uint16_t gmul(const struct drx_bch* b, uint16_t x, uint16_t y)
{
    if (!x || !y)
        return 0;
    return b->exp[b->log[x] + b->log[y]];
}

static inline uint16_t gpow(const struct drx_bch* b, int e)
{
    e %= b->q;
    if (e < 0)
        e += b->q;
    return b->exp[e];
}

/* Multiply GF(2) polynomials given as coefficient arrays (index = degree). */
static int poly2_mul(uint8_t* r, const uint8_t* a, int da, const uint8_t* c, int dc)
{
    memset(r, 0, da + dc + 1);
    for (int i = 0; i <= da; i++)
        if (a[i])
            for (int j = 0; j <= dc; j++)
                r[i + j] ^= c[j];
    return da + dc;
}

/* Minimal polynomial of alpha^i: product of (x + beta) over its conjugates. */
static int minimal_poly(const struct drx_bch* b, int i, uint8_t* out)
{
    uint16_t p[32] = { 1 }; /* coefficients in GF(2^m) */
    int deg = 0, e = i % b->q;
    do {
        uint16_t beta = b->exp[e];
        for (int k = deg + 1; k > 0; k--)
            p[k] = p[k - 1] ^ gmul(b, p[k], beta);
        p[0] = gmul(b, p[0], beta);
        deg++;
        e = (2 * e) % b->q;
    } while (e != i % b->q);
    for (int k = 0; k <= deg; k++)
        out[k] = (uint8_t)(p[k] & 1); /* coefficients are 0 or 1 */
    return deg;
}

static void shl8(uint64_t* r)
{
    for (int w = 0; w < NW - 1; w++)
        r[w] = (r[w] << 8) | (r[w + 1] >> 56);
    r[NW - 1] <<= 8;
}

drx_bch* drx_bch_new(const drx_fec_params* p)
{
    drx_bch* b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->m = p->short_frame ? 14 : 16;
    b->q = (1 << b->m) - 1;
    b->t = p->t;
    b->nbch = p->kldpc;
    const unsigned prim = p->short_frame ? 0x402B  /* x^14+x^5+x^3+x+1 */
                                         : 0x1002D; /* x^16+x^5+x^3+x^2+1 */
    b->exp = malloc(sizeof(uint16_t) * 2 * b->q);
    b->log = malloc(sizeof(int) * (b->q + 1));
    if (!b->exp || !b->log || b->t > MAX_T) {
        drx_bch_free(b);
        return NULL;
    }
    unsigned x = 1;
    for (int i = 0; i < b->q; i++) {
        b->exp[i] = b->exp[i + b->q] = (uint16_t)x;
        b->log[x] = i;
        x <<= 1;
        if (x >> b->m)
            x ^= prim;
    }

    /* g(x) = prod of the minimal polynomials of alpha^(2i+1), i < t */
    uint8_t g[NW * 64 + 1] = { 1 }, tmp[NW * 64 + 1], mp[32];
    int dg = 0;
    for (int i = 1; i < 2 * b->t; i += 2) {
        int dm = minimal_poly(b, i, mp);
        dg = poly2_mul(tmp, g, dg, mp, dm);
        memcpy(g, tmp, dg + 1);
    }
    b->W = dg;

    /* table[v] = (v(x) * x^W) mod g(x), left-aligned in NW words. The
     * register holds W bits with x^(W-1) at bit 255. */
    for (int v = 0; v < 256; v++) {
        uint64_t r[NW] = { 0 };
        uint8_t byte = (uint8_t)v;
        for (int bit = 7; bit >= 0; bit--) {
            int top = (int)(r[0] >> 63) ^ ((byte >> bit) & 1);
            for (int w = 0; w < NW - 1; w++)
                r[w] = (r[w] << 1) | (r[w + 1] >> 63);
            r[NW - 1] <<= 1;
            if (top)
                for (int k = 0; k < dg; k++)
                    if (g[k]) {
                        int pos = 255 - (dg - 1 - k);
                        r[(255 - pos) / 64] ^= 1ULL << (63 - (255 - pos) % 64);
                    }
        }
        memcpy(b->table[0][v], r, sizeof(r));
    }
    for (int s = 1; s < 4; s++) {
        for (int v = 0; v < 256; v++) {
            uint64_t r[NW];
            memcpy(r, b->table[s - 1][v], sizeof(r));
            const uint64_t* t = b->table[0][r[0] >> 56];
            shl8(r);
            for (int w = 0; w < NW; w++)
                b->table[s][v][w] = r[w] ^ t[w];
        }
    }
    return b;
}

void drx_bch_free(drx_bch* b)
{
    if (!b)
        return;
    free(b->exp);
    free(b->log);
    free(b);
}

static int reg_bit(const uint64_t* r, int pos) /* pos 255 = MSB */
{
    int i = 255 - pos;
    return (int)((r[i / 64] >> (63 - i % 64)) & 1);
}

int drx_bch_decode(drx_bch* b, uint8_t* cw)
{
    const int nbytes = b->nbch / 8;
    uint64_t r[NW] = { 0 };
    int i = 0;
    for (; i + 4 <= nbytes; i += 4) {
        const uint32_t top = (uint32_t)(r[0] >> 32) ^ ((uint32_t)cw[i] << 24 | (uint32_t)cw[i + 1] << 16 |
                                                     (uint32_t)cw[i + 2] << 8 | cw[i + 3]);
        const uint64_t* t3 = b->table[3][top >> 24];
        const uint64_t* t2 = b->table[2][(top >> 16) & 255];
        const uint64_t* t1 = b->table[1][(top >> 8) & 255];
        const uint64_t* t0 = b->table[0][top & 255];
        for (int w = 0; w < NW - 1; w++)
            r[w] = (r[w] << 32) | (r[w + 1] >> 32);
        r[NW - 1] <<= 32;
        for (int w = 0; w < NW; w++)
            r[w] ^= t3[w] ^ t2[w] ^ t1[w] ^ t0[w];
    }
    for (; i < nbytes; i++) {
        const uint64_t* t = b->table[0][(r[0] >> 56) ^ cw[i]];
        shl8(r);
        for (int w = 0; w < NW; w++)
            r[w] ^= t[w];
    }
    if (!(r[0] | r[1] | r[2] | r[3]))
        return 0;

    /* The register holds R(x) = c(x) x^W mod g(x), so
     * S_j = c(alpha^j) = R(alpha^j) alpha^(-jW). */
    const int t2 = 2 * b->t;
    uint16_t S[2 * MAX_T + 1];
    for (int j = 1; j <= t2; j += 2) {
        uint16_t s = 0;
        for (int k = 0; k < b->W; k++)
            if (reg_bit(r, 255 - (b->W - 1 - k)))
                s ^= gpow(b, j * k);
        S[j] = gmul(b, s, gpow(b, -j * b->W));
    }
    for (int j = 2; j <= t2; j += 2)
        S[j] = gmul(b, S[j / 2], S[j / 2]);

    /* Berlekamp-Massey */
    uint16_t L[MAX_T + 2] = { 1 }, B[MAX_T + 2] = { 1 }, T[MAX_T + 2];
    int len = 0, shift = 1;
    uint16_t bdisc = 1;
    for (int n = 0; n < t2; n++) {
        uint16_t d = S[n + 1];
        for (int i = 1; i <= len; i++)
            d ^= gmul(b, L[i], S[n + 1 - i]);
        if (!d) {
            shift++;
            continue;
        }
        uint16_t coef = gmul(b, d, b->exp[b->q - b->log[bdisc]]);
        memcpy(T, L, sizeof(T));
        for (int i = 0; i + shift <= b->t + 1; i++)
            L[i + shift] ^= gmul(b, coef, B[i]);
        if (2 * len <= n) {
            len = n + 1 - len;
            memcpy(B, T, sizeof(B));
            bdisc = d;
            shift = 1;
        } else {
            shift++;
        }
    }
    if (len > b->t)
        return -1;

    /* Chien search over the shortened code: an error at degree p (bit
     * nbch-1-p in transmission order) makes L(alpha^-p) zero. */
    /* Terms are tracked as logarithms: term k of step p is
     * log(L_k) - k p (mod q). Zero coefficients are skipped. */
    int pos[MAX_T], nroots = 0;
    int lg[MAX_T + 1], step[MAX_T + 1], nt = 0;
    for (int k = 1; k <= len; k++)
        if (L[k]) {
            lg[nt] = b->log[L[k]];
            step[nt++] = b->q - k % b->q;
        }
    for (int p = 0; p < b->nbch; p++) {
        uint16_t sum = L[0];
        for (int i = 0; i < nt; i++) {
            sum ^= b->exp[lg[i]];
            lg[i] += step[i];
            if (lg[i] >= b->q)
                lg[i] -= b->q;
        }
        if (!sum) {
            if (nroots == len)
                return -1;
            pos[nroots++] = p;
        }
    }
    if (nroots != len)
        return -1;
    for (int i = 0; i < nroots; i++) {
        int bit = b->nbch - 1 - pos[i];
        cw[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
    }
    return nroots;
}
