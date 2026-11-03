/*
 * DVB outer layer, see outer.h.
 */
#include "outer.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SYNC_MISSES 8
#define DEFAULT_TIMEOUT_BITS (64 * OUTER_PKT * 8)

static uint8_t SYN_MUL[16][256]; /* x * a^i, for the RS syndromes */
/* (u x^(16 + m)) mod g(x) for the remainder, 16 coefficients as two words:
 * coefficient k in byte k % 8 of word k / 8 */
static uint64_t REM_T[4][256][2];
static uint8_t GEXP[512], GLOG[256]; /* GF(256), x^8+x^4+x^3+x^2+1 */
static uint8_t PRBS[8 * OUTER_TS];   /* energy dispersal sequence */
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void init_tables(void)
{
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        GEXP[i] = GEXP[i + 255] = (uint8_t)x;
        GLOG[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    GEXP[510] = GEXP[0];
    GEXP[511] = GEXP[1];
    for (int i = 0; i < 16; i++)
        for (int v = 0; v < 256; v++)
            SYN_MUL[i][v] = v ? GEXP[GLOG[v] + i] : 0;
    /* generator g(x) = prod (x - a^i), i = 0..15; g[16] = 1 */
    uint8_t g[17] = { 1 };
    for (int i = 0; i < 16; i++)
        for (int k = 16; k >= 0; k--)
            g[k] = (uint8_t)((k ? g[k - 1] : 0) ^ (g[k] ? GEXP[GLOG[g[k]] + i] : 0));
    for (int u = 0; u < 256; u++) {
        uint8_t t[16];
        for (int k = 0; k < 16; k++)
            t[k] = u && g[k] ? GEXP[GLOG[u] + GLOG[g[k]]] : 0; /* u x^16 = u (g - x^16) */
        for (int m = 0; m < 4; m++) {
            memcpy(REM_T[m][u], t, 16);
            /* times x: shift up, x^16 folds back as above */
            const uint8_t top = t[15];
            memmove(t + 1, t, 15);
            t[0] = 0;
            for (int k = 0; k < 16; k++)
                t[k] ^= top && g[k] ? GEXP[GLOG[top] + GLOG[g[k]]] : 0;
        }
    }
    /* 1 + x^14 + x^15, initial state 100101010000000 */
    unsigned reg = 0x4A80;
    for (int i = 0; i < 8 * OUTER_TS; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            unsigned out = ((reg >> 1) ^ reg) & 1;
            reg = (reg >> 1) | (out << 14);
            byte = (uint8_t)((byte << 1) | out);
        }
        PRBS[i] = byte;
    }
}

/* ---- Reed-Solomon (204, 188), shortened (255, 239), roots a^0..a^15 ----- */

static inline uint8_t gmul(uint8_t a, uint8_t b)
{
    return a && b ? GEXP[GLOG[a] + GLOG[b]] : 0;
}

static inline uint8_t ginv(uint8_t a) { return GEXP[255 - GLOG[a]]; }

int outer_rs_decode(uint8_t* c)
{
    pthread_once(&tables_once, init_tables);
    /* r(x) = c(x) x^16 mod g(x), four bytes a step (the top four
     * coefficients of r plus the next bytes, folded through REM_T); c is a
     * codeword exactly when r = 0, the usual case */
    uint64_t hi = 0, lo = 0; /* coefficients 15..8, 7..0 */
    for (int j = 0; j < OUTER_PKT; j += 4) {
        uint32_t b;
        memcpy(&b, c + j, sizeof(b));
        const uint32_t u = (uint32_t)(hi >> 32) ^ __builtin_bswap32(b);
        hi = (hi << 32 | lo >> 32) ^ REM_T[3][u >> 24][1] ^ REM_T[2][(u >> 16) & 255][1] ^
             REM_T[1][(u >> 8) & 255][1] ^ REM_T[0][u & 255][1];
        lo = (lo << 32) ^ REM_T[3][u >> 24][0] ^ REM_T[2][(u >> 16) & 255][0] ^
             REM_T[1][(u >> 8) & 255][0] ^ REM_T[0][u & 255][0];
    }
    if (!(hi | lo))
        return 0;
    /* syndromes S_i = c(a^i) = r(a^i) a^(-16 i) */
    uint8_t r[16], S[16];
    for (int k = 0; k < 8; k++) {
        r[k] = (uint8_t)(lo >> (8 * k));
        r[8 + k] = (uint8_t)(hi >> (8 * k));
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = 0;
        for (int k = 15; k >= 0; k--)
            v = SYN_MUL[i][v] ^ r[k];
        S[i] = v ? GEXP[GLOG[v] + (255 - 16 * i % 255) % 255] : 0;
    }

    /* Berlekamp-Massey */
    uint8_t L[17] = { 1 }, B[17] = { 1 }, T[17];
    int len = 0, shift = 1;
    uint8_t bd = 1;
    for (int n = 0; n < 16; n++) {
        uint8_t d = S[n];
        for (int i = 1; i <= len; i++)
            d ^= gmul(L[i], S[n - i]);
        if (!d) {
            shift++;
            continue;
        }
        const uint8_t coef = gmul(d, ginv(bd));
        memcpy(T, L, sizeof(T));
        for (int i = 0; i + shift <= 16; i++)
            L[i + shift] ^= gmul(coef, B[i]);
        if (2 * len <= n) {
            len = n + 1 - len;
            memcpy(B, T, sizeof(B));
            bd = d;
            shift = 1;
        } else {
            shift++;
        }
    }
    if (len > 8)
        return -1;

    /* Omega = S(x) L(x) mod x^16 */
    uint8_t W[16] = { 0 };
    for (int i = 0; i < 16; i++)
        for (int k = 0; k <= len && k <= i; k++)
            W[i] ^= gmul(L[k], S[i - k]);

    /* Chien search over the 204 positions, Forney for the values. Byte j
     * is the coefficient of x^(203 - j). */
    int found = 0;
    for (int p = 0; p < OUTER_PKT; p++) {
        const uint8_t xinv = GEXP[(255 - p) % 255]; /* a^-p */
        uint8_t v = 0, xp = 1;
        for (int k = 0; k <= len; k++) {
            v ^= gmul(L[k], xp);
            xp = gmul(xp, xinv);
        }
        if (v)
            continue;
        /* e = X Omega(X^-1) / L'(X^-1), X = a^p */
        uint8_t om = 0, dl = 0;
        xp = 1;
        for (int i = 0; i < 16; i++) {
            om ^= gmul(W[i], xp);
            xp = gmul(xp, xinv);
        }
        xp = 1; /* L'(x) = sum over odd k of L_k x^(k-1) */
        const uint8_t x2 = gmul(xinv, xinv);
        for (int k = 1; k <= len; k += 2) {
            dl ^= gmul(L[k], xp);
            xp = gmul(xp, x2);
        }
        if (!dl)
            return -1;
        c[OUTER_PKT - 1 - p] ^= gmul(gmul(GEXP[p], om), ginv(dl));
        found++;
    }
    return found == len ? found : -1;
}

/* ---- packet sync, deinterleaver, output ------------------------------------ */

/* Forney deinterleaver: branch i = q mod 12 delays by (11 - i) * 17 uses of
 * the branch, i.e. byte q comes out (11 - i) * 204 bytes after it went in.
 * One ring of the input instead of 12 FIFOs: output q = input q - DELAY[q
 * mod 204]. */
static uint16_t DELAY[OUTER_PKT];
static pthread_once_t delay_once = PTHREAD_ONCE_INIT;

static void init_delay(void)
{
    for (int j = 0; j < OUTER_PKT; j++)
        DELAY[j] = (uint16_t)((OUTER_I - 1 - j % OUTER_I) * OUTER_PKT);
}

int outer_init(struct outer* o)
{
    pthread_once(&tables_once, init_tables);
    pthread_once(&delay_once, init_delay);
    memset(o, 0, sizeof(*o));
    o->timeout_bits = DEFAULT_TIMEOUT_BITS;
    o->sbits = malloc(OUTER_SYNC_BITS);
    o->ring = calloc(OUTER_RING, 1);
    outer_reset(o);
    return o->sbits && o->ring ? 0 : -1;
}

void outer_free(struct outer* o)
{
    free(o->sbits);
    free(o->ring);
}

void outer_reset(struct outer* o)
{
    o->active = 1;
    o->lost = 0;
    o->synced = 0;
    o->nsbits = 0;
    o->bits_since_reset = 0;
    o->misses = 0;
    o->group = -1;
}

static void lose(struct outer* o)
{
    o->active = 0;
    o->synced = 0;
    o->lost = 1;
}

static void output_packet(struct outer* o)
{
    int r = outer_rs_decode(o->pkt);
    if (r > 0)
        o->rs_corrected += r;
    if (o->pkt[0] == 0xB8)
        o->group = 0; /* start of a PRBS period */
    else if (o->group >= 0 && ++o->group == OUTER_SYNC_PKTS)
        o->group = -1; /* missed the inverted sync byte: wait for the next */
    if (o->group < 0)
        return;
    uint8_t ts[OUTER_TS];
    ts[0] = 0x47;
    const uint8_t* prbs = PRBS + o->group * OUTER_TS;
    for (int j = 1; j < OUTER_TS; j++)
        ts[j] = o->pkt[j] ^ prbs[j - 1];
    if (r < 0) {
        ts[1] |= 0x80; /* transport error indicator */
        o->rs_failed++;
    }
    o->packets++;
    o->cb(o->ctx, ts, OUTER_TS);
}

/* Bytes of the interleaved stream, the first one at a packet start after
 * the sync: o->q counts them from there. */
static void bytes_in(struct outer* o, const uint8_t* v, int n)
{
    const uint8_t inv = o->invert ? 0xFF : 0;
    uint8_t* ring = o->ring;
    int i = 0;
    while (i < n) {
        const int p = (int)(o->q % OUTER_PKT);
        if (p == 0) {
            const uint8_t s = v[i] ^ inv;
            if (s != 0x47 && s != 0xB8) {
                if (++o->misses > MAX_SYNC_MISSES) {
                    lose(o);
                    return;
                }
            } else {
                o->misses = 0;
            }
        }
        const int take = OUTER_PKT - p < n - i ? OUTER_PKT - p : n - i;
        const unsigned q = (unsigned)o->q;
        for (int j = 0; j < take; j++) {
            ring[(q + j) & (OUTER_RING - 1)] = v[i + j] ^ inv;
            o->pkt[p + j] = ring[(q + j - DELAY[p + j]) & (OUTER_RING - 1)];
        }
        i += take;
        o->q += take;
        /* the first 11 packets out of the deinterleaver are its initial fill */
        if (p + take == OUTER_PKT && o->q >= (long)OUTER_PKT * OUTER_I)
            output_packet(o);
    }
}

static uint8_t sbyte(const uint8_t* bits, int at)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (uint8_t)(v << 1 | bits[at + i]);
    return v;
}

/* 8 bits (0 / 1 bytes), the first one the MSB, to a byte: the multiply
 * moves byte k to bit 63 - k, without carries into the top byte. */
static inline uint8_t pack8(const uint8_t* bits)
{
    uint64_t x;
    memcpy(&x, bits, sizeof(x));
    return (uint8_t)(((x & 0x0101010101010101ull) * 0x8040201008040201ull) >> 56);
}

/* Synced: bits to whole bytes. */
static int synced_bits(struct outer* o, const uint8_t* bits, int n)
{
    int i = 0;
    for (; i < n && o->nbit; i++) {
        o->byte = (uint8_t)(o->byte << 1 | bits[i]);
        if (++o->nbit == 8) {
            o->nbit = 0;
            bytes_in(o, &o->byte, 1);
        }
    }
    uint8_t buf[256];
    while (n - i >= 8 && o->active) {
        int nb = (n - i) / 8;
        if (nb > (int)sizeof(buf))
            nb = (int)sizeof(buf);
        for (int k = 0; k < nb; k++)
            buf[k] = pack8(bits + i + 8 * k);
        bytes_in(o, buf, nb);
        i += 8 * nb;
    }
    for (; i < n && o->active; i++) {
        o->byte = (uint8_t)(o->byte << 1 | bits[i]);
        o->nbit++;
    }
    return n;
}

void outer_bits(struct outer* o, const uint8_t* bits, int n)
{
    for (int i = 0; i < n && o->active; i++) {
        if (o->synced) {
            synced_bits(o, bits + i, n - i);
            return;
        }
        o->sbits[o->nsbits++] = bits[i];
        o->bits_since_reset++;
        if (o->nsbits < OUTER_SYNC_BITS)
            continue;
        /* try every bit offset within one packet */
        for (int off = 0; off < OUTER_PKT * 8; off++) {
            int matches = 0, b8 = 0;
            for (int m = 0; m < OUTER_SYNC_PKTS; m++) {
                const uint8_t v = sbyte(o->sbits, off + m * OUTER_PKT * 8);
                matches += v == 0x47 || v == 0xB8;
                b8 += v == 0xB8;
            }
            if (matches >= OUTER_SYNC_PKTS - 1) {
                o->synced = 1;
                o->invert = b8 > OUTER_SYNC_PKTS / 2;
                o->q = 0;
                o->nbit = 0;
                /* replay the buffered bits from the sync byte on */
                synced_bits(o, o->sbits + off, o->nsbits - off);
                break;
            }
        }
        if (!o->synced) {
            /* keep the last packet's worth of bits and slide on */
            const int keep = OUTER_PKT * 8;
            memmove(o->sbits, o->sbits + o->nsbits - keep, keep);
            o->nsbits = keep;
            if (o->bits_since_reset > o->timeout_bits)
                lose(o);
        }
    }
}
