/*
 * DVB-T2 (EN 302 755) receiver, see dvbt2.h.
 *
 * Acquisition: the P1 symbol is found by its two frequency-shifted guard
 * parts (correlations at lags 542 and 482 with the shift removed), which
 * also give the timing and the carrier offset within half a 1K carrier;
 * its FFT gives the integer offset and the S1 / S2 fields (FFT size, guard
 * interval group). The guard interval is then picked by cyclic prefix
 * correlation on the next symbols.
 *
 * Each T2-frame is demodulated symbol by symbol into a frame cell buffer
 * (equalised cells and channel state, frequency deinterleaved):
 *   - P2 symbols: channel from the dense P2 pilots, interpolated; it also
 *     seeds the estimates used for the data symbols;
 *   - after the P2 symbols, L1-pre (BPSK, 16K LDPC 1/4 shortened and
 *     punctured) and L1-post (16K LDPC 1/2) are decoded: pilot pattern,
 *     number of symbols, PLPs and their position in the frame;
 *   - data and frame closing symbols: scattered and edge pilots (all on
 *     k = 0 mod Dx) refresh a channel grid held across the Dy symbols of
 *     the pattern, interpolated in frequency with the mean phase slope
 *     taken out; continual pilots give the common phase error (carrier
 *     loop) and the sample clock drift (window position, resampler).
 * At the end of the frame the PLP cells are time and cell deinterleaved,
 * demapped (rotated constellations: I and the cyclically delayed Q of a
 * point are derotated together, each weighted by its own channel state),
 * bit deinterleaved, LDPC and BCH decoded, and the baseband frames give
 * the TS.
 *
 * Not handled: MISO, T2-Lite, FEF, Type 2 PLPs and sub-slicing, time
 * interleaving across several T2-frames, tone reservation in the frame
 * closing symbol.
 */
#include "dvbrx/dvbt2.h"
#include "dvbrx/bb.h"
#include "dvbrx/fec.h"
#include "dvbrx/ldpc.h"
#include "fft.h"
#include "resamp.h"
#include "t2_tables.h"

#include <complex.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float complex cf;

/* 256-bit vectors (GCC vector extensions): 8 floats or 4 complex values */
typedef float v8f __attribute__((vector_size(32)));
typedef int32_t v8i __attribute__((vector_size(32)));

static inline v8f ldf(const float* p)
{
    v8f v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void stf(float* p, v8f v)
{
    memcpy(p, &v, sizeof(v));
}

static inline v8f vsplat(float a)
{
    return (v8f){ a, a, a, a, a, a, a, a };
}

/* in each complex value: (re, im) -> (im, re), (re, re), (im, im) */
static inline v8f swap_ri(v8f a)
{
    return __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6);
}

static inline v8f dup_re(v8f a)
{
    return __builtin_shufflevector(a, a, 0, 0, 2, 2, 4, 4, 6, 6);
}

static inline v8f dup_im(v8f a)
{
    return __builtin_shufflevector(a, a, 1, 1, 3, 3, 5, 5, 7, 7);
}

static const v8i NEG_RE = { INT32_MIN, 0, INT32_MIN, 0, INT32_MIN, 0, INT32_MIN, 0 };
static const v8i NEG_IM = { 0, INT32_MIN, 0, INT32_MIN, 0, INT32_MIN, 0, INT32_MIN };

static inline v8f vxor(v8f a, v8i m)
{
    return (v8f)((v8i)a ^ m);
}

/* complex products a b and a conj(b) */
static inline v8f cmul(v8f a, v8f b)
{
    return a * dup_re(b) + swap_ri(a) * vxor(dup_im(b), NEG_RE);
}

static inline v8f cmulc(v8f a, v8f b)
{
    return a * dup_re(b) + swap_ri(a) * vxor(dup_im(b), NEG_IM);
}

#define P1_LEN 2048
#define P1_C 542
#define P1_B 482
#define P1_SCAN 65536        /* samples scanned for a P1 at a time */
#define P1_MIN 0.45f         /* normalised P1 correlation */
#define P1_IMAX 64           /* integer offset search, 1K carriers */
#define CLOCK_ACQ 8          /* data symbols before the clock estimate is used */
#define CLOCK_SETTLE 20      /* data symbols before frames are decoded */
#define KF 0.05              /* carrier loop gain, per symbol */
#define KF_ACQ 0.5           /* the same, on the first data symbols */
/* LDPC iterations: near the threshold, a batch of frames runs until its
 * slowest frame converges, so the cap sets the cost (32K 256-QAM 2/3 at
 * 18.5 dB: 6.8 s of CPU for 3 s of signal at 50, 4.5 s at 30, the same
 * frames decoded). When frames queue up for the FEC threads, a lower cap
 * loses a few more blocks instead of dropping input samples (whole frames
 * and a resynchronisation). */
#ifndef LDPC_ITER
#define LDPC_ITER 30
#endif
#define LDPC_ITER_BUSY 15
#define MAX_FEC 1024         /* FEC blocks of one PLP per frame */
#define KPAD 27848           /* carrier arrays: 32K extended (27841), whole vectors */

enum { ST_SEARCH, ST_FRAME };

/* ---- static parameters (EN 302 755 clauses 8, 9) --------------------------- */

static const int DXT[9] = { 0, 3, 6, 6, 12, 12, 24, 24, 6 };
static const int DYT[9] = { 0, 4, 2, 4, 2, 4, 2, 4, 16 };

/* guard intervals as fractions, in the L1-pre GUARD_INTERVAL order */
static const int GI_NUM[7] = { 1, 1, 1, 1, 1, 19, 19 };
static const int GI_DEN[7] = { 32, 16, 8, 4, 128, 128, 256 };

static int fft_idx(int n)
{
    return n == 1024 ? 0 : n == 2048 ? 1 : n == 4096 ? 2 : n == 8192 ? 3 : n == 16384 ? 4 : 5;
}

static const int NP2_T[6] = { 16, 8, 4, 2, 1, 1 };
static const int KTOT_N[6] = { 853, 1705, 3409, 6817, 13633, 27265 };
static const int KTOT_E[6] = { 853, 1705, 3409, 6913, 13921, 27841 };
/* reserved carriers for PAPR (annex H) */
static const int NTR_T[6] = { 10, 18, 36, 72, 144, 288 };
static const int KMOD_T[6] = { 1632, 1632, 3264, 6528, 13056, 0 };
static const int CPGROUPS_T[6] = { 1, 2, 3, 4, 5, 6 };
/* table 50: active cells of the frame closing symbol, [fft][ext][pp] */
static const int CFC_T[6][2][9] = {
    { { 0, 402, 654, 490, 707, 544, 0, 0, 0 }, { 0 } },
    { { 0, 804, 1309, 980, 1415, 1088, 0, 1396, 0 }, { 0 } },
    { { 0, 1609, 2619, 1961, 2831, 2177, 0, 2792, 0 }, { 0 } },
    { { 0, 3218, 5238, 3922, 5662, 4354, 0, 5585, 0 }, { 0, 3264, 5312, 3978, 5742, 4416, 0, 5664, 0 } },
    { { 0, 6437, 10476, 7845, 11324, 8709, 11801, 11170, 0 },
      { 0, 6573, 10697, 8011, 11563, 8893, 12051, 11406, 0 } },
    { { 0, 0, 20952, 0, 22649, 0, 23603, 0, 0 }, { 0, 0, 21395, 0, 23127, 0, 24102, 0, 0 } },
};

/* frequency interleaver: PRBS taps and bit permutations per FFT size */
static const int FI_TAPS[6][6] = { { 0, 4 }, { 0, 3 }, { 0, 2 }, { 0, 1, 4, 6 }, { 0, 1, 4, 5, 9, 11 }, { 0, 1, 2, 12 } };
static const int FI_NTAPS[6] = { 2, 2, 2, 4, 6, 4 };
static const int FI_EVEN[6][14] = {
    { 8, 7, 6, 5, 0, 1, 2, 3, 4 },
    { 4, 3, 9, 6, 2, 8, 1, 5, 7, 0 },
    { 6, 3, 0, 9, 4, 2, 1, 8, 5, 10, 7 },
    { 7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5 },
    { 9, 7, 6, 10, 12, 5, 1, 11, 0, 2, 3, 4, 8 },
    { 7, 13, 3, 4, 9, 2, 12, 11, 1, 8, 10, 0, 5, 6 },
};
static const int FI_ODD[6][14] = {
    { 6, 8, 7, 4, 1, 0, 5, 2, 3 },
    { 6, 9, 4, 8, 5, 1, 0, 7, 2, 3 },
    { 5, 9, 1, 4, 3, 0, 8, 10, 7, 2, 6 },
    { 11, 4, 9, 3, 1, 2, 5, 0, 6, 7, 10, 8 },
    { 6, 8, 10, 12, 2, 0, 4, 1, 11, 3, 5, 9, 7 },
    { 7, 13, 3, 4, 9, 2, 12, 11, 1, 8, 10, 0, 5, 6 },
};

/* bit interleaver (clause 6.1.3) and demultiplexer (6.2.1) */
static const int TWIST16N[8] = { 0, 0, 2, 4, 4, 5, 7, 7 };
static const int TWIST64N[12] = { 0, 0, 2, 2, 3, 4, 4, 5, 5, 7, 8, 9 };
static const int TWIST256N[16] = { 0, 2, 2, 2, 2, 3, 7, 15, 16, 20, 22, 22, 27, 27, 28, 32 };
static const int TWIST16S[8] = { 0, 0, 0, 1, 7, 20, 20, 21 };
static const int TWIST64S[12] = { 0, 0, 0, 2, 2, 2, 3, 3, 3, 6, 7, 7 };
static const int TWIST256S[8] = { 0, 0, 0, 1, 7, 20, 20, 21 };
static const int MUX16[8] = { 7, 1, 4, 2, 5, 3, 6, 0 };
static const int MUX64[12] = { 11, 7, 3, 10, 6, 2, 9, 5, 1, 8, 4, 0 };
static const int MUX256[16] = { 15, 1, 13, 3, 8, 11, 9, 5, 10, 6, 4, 7, 12, 2, 14, 0 };
static const int MUX16_35[8] = { 0, 5, 1, 2, 4, 7, 3, 6 };
static const int MUX64_35[12] = { 2, 7, 6, 9, 0, 3, 1, 8, 4, 11, 5, 10 };
static const int MUX256_35[16] = { 2, 11, 3, 4, 0, 9, 1, 8, 10, 13, 7, 14, 6, 15, 5, 12 };
static const int MUX256_23[16] = { 7, 2, 9, 0, 4, 6, 13, 3, 14, 10, 15, 5, 8, 12, 11, 1 };
static const int MUX256S[8] = { 7, 3, 1, 5, 2, 6, 4, 0 };

static const char* const PLP_RATES[8] = { "1_2", "3_5", "2_3", "3_4", "4_5", "5_6", "1_3", "2_5" };
/* rotation angles (clause 6.3), degrees */
static const double ROT_DEG[4] = { 29.0, 16.8, 8.6, 3.576334375 };

/* ---- configuration from P1 and L1 ------------------------------------------- */

struct l1pre {
    int type, bwt_ext, s1, s2, repetition, guard, papr, l1_mod, l1_cod, l1_fec_type;
    int post_size, post_info_size, pilot_pattern, tx_id, cell_id, network_id, t2_system_id;
    int num_t2_frames, num_data_symbols, regen, post_extension, num_rf, current_rf;
    int t2_version, post_scrambled, base_lite;
};

struct plp {
    int id, type, payload_type, ff, first_rf, first_frame, group, cod, mod, rotation, fec_type;
    int num_blocks_max, frame_interval, time_il_length, time_il_type, in_band_a, in_band_b;
    int mode, static_flag, static_padding;
    int start, num_blocks; /* dynamic */
};

#define MAX_PLP 32

/* Carrier layout for one (FFT size, carrier mode, pilot pattern). */
struct layout {
    int n, ktot, kmax, kc, kext, dx, dy, pp, ext, tr, fc;
    float asp, acp, ap2;
    float* w;            /* 1 - 2 w(k): pilot sign of carrier k before pn(l) */
    int ncp;
    int* cp;             /* continual pilots */
    int* cp_res;         /* (k - Kext) mod Dx Dy of each */
    uint8_t* cp_is_grid; /* on k = 0 mod Dx */
    int np2, cp2, cdata, nfc, cfc;
    int* p2_data;        /* data carriers of P2 symbols */
    uint8_t* p2_pilot;   /* is k a P2 pilot */
    int* data[16];       /* data carriers by l mod Dy */
    int* fc_data;
    /* frequency deinterleavers (inverse of H(q), clause 8.5): the data
     * carrier of cell q, even and odd symbols */
    int *hp2e, *hp2o, *hde, *hdo, *hfce, *hfco;
    int ngrid;           /* grid points k = j Dx, j < ngrid */
};

static void free_layout(struct layout* L)
{
    free(L->w);
    free(L->cp);
    free(L->cp_res);
    free(L->cp_is_grid);
    free(L->p2_data);
    free(L->p2_pilot);
    for (int i = 0; i < 16; i++)
        free(L->data[i]);
    free(L->fc_data);
    free(L->hp2e);
    free(L->hp2o);
    free(L->hde);
    free(L->hdo);
    free(L->hfce);
    free(L->hfco);
    memset(L, 0, sizeof(*L));
}

/* Frequency interleaver permutations for nmax cells (clause 8.5). */
static void make_fi(int n, int nmax, int* he, int* ho)
{
    const int fi = fft_idx(n), nr = (int)log2(n) - 1;
    int lfsr = 0, qe = 0, qo = 0;
    for (int i = 0; i < n; i++) {
        if (i < 2) {
            lfsr = 0;
        } else if (i == 2) {
            lfsr = 1;
        } else {
            int b = 0;
            for (int t = 0; t < FI_NTAPS[fi]; t++)
                b ^= (lfsr >> FI_TAPS[fi][t]) & 1;
            lfsr = (lfsr >> 1) | (b << (nr - 1));
        }
        int e = 0, o = 0;
        for (int j = 0; j < nr; j++) {
            e |= ((lfsr >> j) & 1) << FI_EVEN[fi][j];
            o |= ((lfsr >> j) & 1) << FI_ODD[fi][j];
        }
        e += (i & 1) * (n / 2);
        o += (i & 1) * (n / 2);
        if (e < nmax)
            he[qe++] = e;
        if (o < nmax)
            ho[qo++] = o;
    }
    if (n == 32768) /* one permutation: its inverse on even symbols */
        for (int j = 0; j < nmax; j++)
            he[ho[j]] = j;
}

/* Inverse of the permutation h of 0..nmax - 1 (in place). */
static int invert_perm(int* h, int nmax)
{
    int* t = malloc(sizeof(int) * nmax);
    if (!t)
        return -1;
    for (int j = 0; j < nmax; j++)
        t[h[j]] = j;
    memcpy(h, t, sizeof(int) * nmax);
    free(t);
    return 0;
}

static int make_layout(struct layout* L, int n, int ext, int pp, int tr, int guard)
{
    memset(L, 0, sizeof(*L));
    const int fi = fft_idx(n);
    L->n = n;
    L->ext = ext && n >= 8192;
    L->ktot = L->ext ? KTOT_E[fi] : KTOT_N[fi];
    L->kmax = L->ktot - 1;
    L->kc = L->kmax / 2;
    L->kext = (L->ktot - KTOT_N[fi]) / 2;
    L->pp = pp;
    L->dx = DXT[pp];
    L->dy = DYT[pp];
    L->tr = tr;
    L->asp = pp <= 2 ? 4.0f / 3 : pp <= 4 ? 7.0f / 4 : 7.0f / 3;
    L->acp = n <= 2048 ? 4.0f / 3 : n == 4096 ? (float)(4 * M_SQRT2 / 3) : 8.0f / 3;
    L->ap2 = n == 32768 ? sqrtf(37.0f) / 5 : sqrtf(31.0f) / 5;
    L->np2 = NP2_T[fi];
    L->ngrid = L->kmax / L->dx + 1;
    /* frame closing symbol when Dx Dy >= 1 / guard fraction (SISO, not PP8) */
    L->fc = pp != 8 && L->dx * L->dy * GI_NUM[guard] >= GI_DEN[guard];
    const int nk = L->ktot;
    L->w = malloc(sizeof(float) * nk);
    L->cp = malloc(sizeof(int) * 512);
    L->cp_res = malloc(sizeof(int) * 512);
    L->cp_is_grid = malloc(512);
    L->p2_pilot = calloc(nk, 1);
    uint8_t* special = calloc(nk, 1);
    int ok = L->w && L->cp && L->cp_res && L->cp_is_grid && L->p2_pilot && special;
    if (!ok)
        goto fail;
    /* symbol-level PRBS, offset by Kext in normal carrier mode */
    {
        unsigned reg = 0x7FF;
        const int skip = L->ext ? 0 : (KTOT_E[fi] - KTOT_N[fi]) / 2;
        for (int i = 0; i < skip + nk; i++) {
            if (i >= skip)
                L->w[i - skip] = (reg & 1) ? -1.0f : 1.0f;
            const unsigned b = ((reg >> 2) ^ reg) & 1;
            reg = (reg >> 1) | (b << 10);
        }
    }
    /* continual pilots */
    for (size_t g = 0; g < sizeof(T2_CP_GROUPS) / sizeof(T2_CP_GROUPS[0]); g++) {
        const struct t2_cp_group* G = &T2_CP_GROUPS[g];
        if (G->pp != pp || G->group > CPGROUPS_T[fi])
            continue;
        for (int i = 0; i < G->n; i++) {
            const int k = KMOD_T[fi] ? G->k[i] % KMOD_T[fi] : G->k[i];
            if (k <= L->kmax && !special[k] && L->ncp < 512) {
                special[k] = 1;
                L->cp[L->ncp++] = k;
            }
        }
    }
    if (L->ext)
        for (size_t g = 0; g < sizeof(T2_CP_EXT) / sizeof(T2_CP_EXT[0]); g++) {
            const struct t2_cp_ext* G = &T2_CP_EXT[g];
            if (G->pp != pp || G->fft != n)
                continue;
            for (int i = 0; i < G->n; i++)
                if (G->k[i] <= L->kmax && !special[G->k[i]] && L->ncp < 512) {
                    special[G->k[i]] = 1;
                    L->cp[L->ncp++] = G->k[i];
                }
        }
    /* sort them */
    for (int i = 1; i < L->ncp; i++)
        for (int j = i; j > 0 && L->cp[j] < L->cp[j - 1]; j--) {
            const int t = L->cp[j];
            L->cp[j] = L->cp[j - 1];
            L->cp[j - 1] = t;
        }
    for (int i = 0; i < L->ncp; i++) {
        L->cp_is_grid[i] = L->cp[i] % L->dx == 0;
        int r = (L->cp[i] - L->kext) % (L->dx * L->dy);
        L->cp_res[i] = r < 0 ? r + L->dx * L->dy : r;
    }
    /* P2 symbols: pilots every 3rd (32K: 6th) carrier, and the extended
     * edges; tone reservation */
    {
        const int step = n == 32768 ? 6 : 3;
        uint8_t* p2 = L->p2_pilot;
        for (int k = 0; k < nk; k++)
            p2[k] = k % step == 0 || k < L->kext || k > L->kmax - L->kext;
        uint8_t* res = calloc(nk, 1);
        if (!res)
            goto fail;
        {
            /* the P2 reserved carriers (table H.1) are always reserved */
            const int16_t* map = fi == 0 ? T2_P2_PAPR_1K : fi == 1 ? T2_P2_PAPR_2K : fi == 2 ? T2_P2_PAPR_4K
                               : fi == 3 ? T2_P2_PAPR_8K : fi == 4 ? T2_P2_PAPR_16K : T2_P2_PAPR_32K;
            const int nm = NTR_T[fi];
            for (int i = 0; i < nm; i++)
                if (map[i] + L->kext <= L->kmax)
                    res[map[i] + L->kext] = 1;
        }
        int c = 0;
        for (int k = 0; k < nk; k++)
            c += !p2[k] && !res[k];
        L->cp2 = c;
        L->p2_data = malloc(sizeof(int) * c);
        if (!L->p2_data) {
            free(res);
            goto fail;
        }
        c = 0;
        for (int k = 0; k < nk; k++)
            if (!p2[k] && !res[k])
                L->p2_data[c++] = k;
        free(res);
    }
    /* data symbols: scattered pilots by l mod Dy, edges, continual, TR */
    {
        uint8_t* m = malloc(nk);
        if (!m)
            goto fail;
        const int16_t* tmap = fi == 0 ? T2_TR_PAPR_1K : fi == 1 ? T2_TR_PAPR_2K : fi == 2 ? T2_TR_PAPR_4K
                            : fi == 3 ? T2_TR_PAPR_8K : fi == 4 ? T2_TR_PAPR_16K : T2_TR_PAPR_32K;
        L->cdata = -1;
        for (int p = 0; p < L->dy; p++) {
            memcpy(m, special, nk);
            for (int k = 0; k < nk; k++) {
                int r = (k - L->kext) % (L->dx * L->dy);
                if (r < 0)
                    r += L->dx * L->dy;
                if (r == L->dx * p)
                    m[k] = 1;
            }
            m[0] = m[L->kmax] = 1;
            if (tr) {
                const int shift = L->dx * ((p + L->kext / L->dx) % L->dy);
                for (int i = 0; i < (NTR_T[fi]); i++)
                    if (tmap[i] + shift <= L->kmax)
                        m[tmap[i] + shift] = 1;
            }
            int c = 0;
            for (int k = 0; k < nk; k++)
                c += !m[k];
            if (L->cdata < 0)
                L->cdata = c;
            if (c != L->cdata || !(L->data[p] = malloc(sizeof(int) * c))) {
                free(m);
                goto fail;
            }
            c = 0;
            for (int k = 0; k < nk; k++)
                if (!m[k])
                    L->data[p][c++] = k;
        }
        /* frame closing symbol: pilots on k = 0 mod Dx and the edges */
        if (L->fc) {
            memset(m, 0, nk);
            for (int k = 0; k < nk; k += L->dx)
                m[k] = 1;
            m[0] = m[L->kmax] = 1;
            if ((n == 1024 && (pp == 4 || pp == 5)) || (n == 2048 && pp == 7))
                m[L->kmax - 1] = 1;
            if (tr) { /* the P2 reserved carriers */
                const int16_t* map = fi == 0 ? T2_P2_PAPR_1K : fi == 1 ? T2_P2_PAPR_2K : fi == 2 ? T2_P2_PAPR_4K
                                   : fi == 3 ? T2_P2_PAPR_8K : fi == 4 ? T2_P2_PAPR_16K : T2_P2_PAPR_32K;
                for (int i = 0; i < (NTR_T[fi]); i++)
                    if (map[i] + L->kext <= L->kmax)
                        m[map[i] + L->kext] = 1;
            }
            int c = 0;
            for (int k = 0; k < nk; k++)
                c += !m[k];
            L->nfc = c;
            L->cfc = CFC_T[fi][L->ext][pp] - (tr ? NTR_T[fi] : 0);
            L->fc_data = malloc(sizeof(int) * c);
            if (!L->fc_data || L->cfc <= 0) {
                free(m);
                goto fail;
            }
            c = 0;
            for (int k = 0; k < nk; k++)
                if (!m[k])
                    L->fc_data[c++] = k;
        }
        free(m);
    }
    L->hp2e = malloc(sizeof(int) * n);
    L->hp2o = malloc(sizeof(int) * n);
    L->hde = malloc(sizeof(int) * n);
    L->hdo = malloc(sizeof(int) * n);
    L->hfce = malloc(sizeof(int) * n);
    L->hfco = malloc(sizeof(int) * n);
    if (!L->hp2e || !L->hp2o || !L->hde || !L->hdo || !L->hfce || !L->hfco)
        goto fail;
    make_fi(n, L->cp2, L->hp2e, L->hp2o);
    make_fi(n, L->cdata, L->hde, L->hdo);
    if (L->fc)
        make_fi(n, L->nfc, L->hfce, L->hfco);
    if (invert_perm(L->hp2e, L->cp2) || invert_perm(L->hp2o, L->cp2) || invert_perm(L->hde, L->cdata) ||
        invert_perm(L->hdo, L->cdata) ||
        (L->fc && (invert_perm(L->hfce, L->nfc) || invert_perm(L->hfco, L->nfc))))
        goto fail;
    free(special);
    return 0;
fail:
    free(special);
    free_layout(L);
    return -1;
}

/* ---- PRBS of the BB scrambler (1 + x^14 + x^15) ------------------------------ */

static void prbs15(unsigned reg, int n, uint8_t* out)
{
    for (int i = 0; i < n; i++) {
        const unsigned b = ((reg >> 1) ^ reg) & 1;
        reg = (reg >> 1) | (b << 14);
        out[i] = (uint8_t)b;
    }
}

/* ---- P1 --------------------------------------------------------------------- */

struct p1tab {
    cf rot[1024];            /* e^(-2 pi j i / 1024): the P1 frequency shift */
    float scr[384];          /* scrambling, +-1 */
    float s1[8][64];         /* expected DBPSK sign changes, +-1 (S1 part) */
    float s2[16][256];
};

static void make_p1(struct p1tab* t)
{
    uint8_t b[384];
    for (int i = 0; i < 1024; i++)
        t->rot[i] = cexpf(-I * (float)fmod(2 * M_PI / 1024 * i, 2 * M_PI));
    prbs15(0x4E46, 384, b); /* 100111001000110 */
    for (int i = 0; i < 384; i++)
        t->scr[i] = b[i] ? -1.0f : 1.0f;
    for (int v = 0; v < 8; v++)
        for (int i = 0; i < 64; i++)
            t->s1[v][i] = (T2_S1_PATTERNS[v * 8 + i / 8] >> (7 - i % 8)) & 1 ? -1.0f : 1.0f;
    for (int v = 0; v < 16; v++)
        for (int i = 0; i < 256; i++)
            t->s2[v][i] = (T2_S2_PATTERNS[v * 32 + i / 8] >> (7 - i % 8)) & 1 ? -1.0f : 1.0f;
}

/* ---- receiver state ----------------------------------------------------------- */

/* One frame's PLP for the FEC thread: its signalling and its cells after
 * time deinterleaving. */
#define NFEC 3         /* FEC threads: near the threshold LDPC needs more than two cores */
#define NJOB (NFEC + 1) /* frame buffers: one being filled, one per FEC thread */

struct fecjob {
    int state; /* JOB_FREE, JOB_READY (filled), JOB_BUSY (being decoded) */
    uint64_t seq;
    struct plp q;
    cf* buf;
    float* csi;
    long cap, total;
    /* decoded: BBFRAMEs of kb bytes, BCH success, MER terms per FEC block */
    uint8_t* bb;
    uint8_t* ok;
    float* err;
    float* sig;
    int bbcap, nblocks, kb;
    drx_dvbt2_cb cb; /* where its TS goes */
    void* ctx;
};
enum { JOB_FREE, JOB_READY, JOB_BUSY };

/* A FEC thread and its decoders (set up for the PLP's code and modulation). */
struct fecwork {
    struct drx_dvbt2* d;
    pthread_t th;
    int started;
    drx_ldpc* ldpc;
    drx_bch* bch;
    drx_fec_params fec;
    int* bitmap;   /* bit deinterleaver: interleaver output -> codeword */
    int bitmap_key;
    int* cellperm; /* L0(q) */
    int cellperm_n;
    /* demapper: per cell of a FEC block (padded for whole vectors) its I, Q,
     * channel state, then the derotated point and per-axis weights; the 8-bit
     * soft values by bit of the cell (planes of `stride` bytes, a multiple of
     * 32 cells); QPSK: the plane byte of each codeword bit */
    float *ci, *cq, *cc, *ck;
    int8_t* planes;
    int stride;
    uint16_t* llrsrc; /* QPSK */
    /* 16-QAM and up: column c of the bit interleaver (nrow bits, twist
     * coltw[c]) is the soft values of one bit of one of the cells of the
     * demultiplexer words: a plane (cell order, or with two cells per word,
     * even cells then odd cells) from colsrc[c] */
    int ncol, nrow, colsrc[16], coltw[16], split;
    int8_t* il; /* interleaver columns, then the codeword (parity interleaving) */
    int8_t* llr[64];
    uint8_t* cw[64];
};

struct drx_dvbt2 {
    double in_rate, elem_rate;
    int want_plp;
    struct resamp rs;
    double step_nominal;
    _Atomic double step;
    double cur_step;
    double clock;

    cf* buf;
    int nbuf, cap, pos;
    int state;

    struct p1tab p1;
    double complex *p1c1, *p1c2; /* find_p1 prefix sums */
    double complex *p1c1i, *p1c2i; /* the same for an inverted spectrum */
    int inv; /* input conjugated: spectrum inverted (cable networks) */
    double* p1e;
    struct fft fft1k, fft;
    cf* win; /* FFT buffer, 32K */

    /* frame structure */
    int n, g, sym_len, s1, s2;
    int guard_idx;  /* L1-pre GUARD_INTERVAL */
    int tr_guess;   /* tone reservation, from the last L1-pre */
    struct layout L;
    int have_layout, layout_key;
    struct l1pre pre;
    int have_pre;
    struct plp plps[MAX_PLP];
    int num_plp, sel; /* PLP decoded, index in plps */
    int ldata, lf;   /* data symbols, symbols per frame (after P1) */
    int l;           /* symbol of the frame being demodulated */
    int frame_ok;    /* L1 decoded for this frame */
    int tracked_start; /* tracked data symbols when this frame's data began */

    /* carrier, timing */
    double f, phase;
    double tau, drift;
    int tracked;

    /* channel */
    cf* x;     /* carriers of the current symbol */
    cf* hk;    /* channel per carrier (P2) */
    cf* hd;    /* channel per carrier (data symbols, interpolated from the grid),
                  then the equalised data carriers */
    float* hc; /* |h|^2 of the data carriers */
    float* h2; /* store_cells: |h|^2 per carrier, twice (KPAD values per array) */
    cf* grid;  /* channel on k = j Dx */
    cf* hcp;   /* channel at the continual pilots (previous symbol) */
    cf* cpp;   /* continual pilots of the previous symbol (clock) */
    int have_cpp;
    cf slope_back;     /* e^(-j phi): mean phase step over Dx carriers */
    cf slope_tab[24];  /* e^(j phi r / Dx) */

    /* frame cells: equalised value and channel state */
    cf* cells;
    float* csi;
    int ncells_cap;
    float mer_err, mer_sig;

    /* PLP decoding */
    drx_ldpc *ldpc_pre, *ldpc_post;
    drx_bch *bch_pre, *bch_post;
    drx_fec_params fec_pre, fec_post;
    int8_t* llr[64];
    uint8_t* cw[64];
    drx_bbdec* bb;
    uint8_t* ts;
    uint8_t* bbframe;

    drx_dvbt2_cb cb;
    void* ctx;
    drx_dvbt2_stats st;

    /* FEC threads: the PLP of each frame (TI-deinterleaved cells) is
     * decoded there while the next frames are demodulated; frames are
     * decoded in parallel, their BBFRAMEs turned into TS in order (bb, ts,
     * bbframe, mer_*, the FEC statistics: by the thread whose frame is the
     * next one, emit_seq). */
    struct fecwork fw[NFEC];
    int fec_stop, fec_ok;
    pthread_mutex_t fec_lock;
    pthread_cond_t fec_cv;
    struct fecjob job[NJOB];
    uint64_t next_seq, emit_seq;
};

/* ---- setup ----------------------------------------------------------------- */

static void* fec_thread(void* arg);

drx_dvbt2* drx_dvbt2_new(double sample_rate, double bandwidth, int plp_id)
{
    if (bandwidth < 1e6 || sample_rate < bandwidth)
        return NULL;
    drx_dvbt2* d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->in_rate = sample_rate;
    /* elementary period: 7/64 us at 8 MHz, i.e. 8/7 of the bandwidth
     * (1.7 MHz: 131/71 MHz) */
    d->elem_rate = bandwidth < 2e6 ? 131e6 / 71 : bandwidth * 8.0 / 7.0;
    d->want_plp = plp_id;
    d->step_nominal = sample_rate / d->elem_rate;
    d->step = d->step_nominal;
    d->cap = 1 << 20;
    d->buf = malloc(sizeof(cf) * d->cap);
    d->win = malloc(sizeof(cf) * 32768);
    d->x = calloc(KPAD, sizeof(cf));
    d->hk = calloc(KPAD, sizeof(cf));
    d->hd = calloc(KPAD, sizeof(cf));
    d->hc = calloc(KPAD, sizeof(float));
    d->h2 = calloc(2 * KPAD, sizeof(float));
    d->grid = malloc(sizeof(cf) * (27841 / 3 + 2));
    d->hcp = malloc(sizeof(cf) * 512);
    d->cpp = malloc(sizeof(cf) * 512);
    d->p1c1 = malloc(sizeof(double complex) * (P1_SCAN + P1_LEN + 1));
    d->p1c2 = malloc(sizeof(double complex) * (P1_SCAN + P1_LEN + 1));
    d->p1e = malloc(sizeof(double) * (P1_SCAN + P1_LEN + 1));
    d->p1c1i = malloc(sizeof(double complex) * (P1_SCAN + P1_LEN + 1));
    d->p1c2i = malloc(sizeof(double complex) * (P1_SCAN + P1_LEN + 1));
    d->bb = drx_bbdec_new();
    d->ts = malloc(64800 / 8 + 188);
    d->bbframe = malloc(64800 / 8);
    int ok = d->p1c1 && d->p1c2 && d->p1e && d->p1c1i && d->p1c2i && d->buf && d->win && d->x && d->hk && d->hd && d->hc && d->h2 &&
             d->grid && d->hcp && d->cpp && d->bb && d->ts && d->bbframe;
    ok = ok && !resamp_init(&d->rs, d->step_nominal, 0.86) && !fft_init(&d->fft1k, 1024);
    ok = ok && !drx_fec_lookup(1, "1_4", &d->fec_pre) && !drx_fec_lookup(1, "1_2", &d->fec_post);
    if (ok) {
        d->ldpc_pre = drx_ldpc_new(&d->fec_pre, "1_4");
        d->ldpc_post = drx_ldpc_new(&d->fec_post, "1_2");
        d->bch_pre = drx_bch_new(&d->fec_pre);
        d->bch_post = drx_bch_new(&d->fec_post);
        ok = d->ldpc_pre && d->ldpc_post && d->bch_pre && d->bch_post;
    }
    for (int i = 0; ok && i < drx_ldpc_lanes(); i++) {
        d->llr[i] = malloc(64800);
        d->cw[i] = malloc(64800 / 8);
        ok = d->llr[i] && d->cw[i];
        for (int w = 0; ok && w < NFEC; w++) {
            d->fw[w].llr[i] = malloc(64800);
            d->fw[w].cw[i] = malloc(64800 / 8);
            ok = d->fw[w].llr[i] && d->fw[w].cw[i];
        }
    }
    if (ok) {
        pthread_mutex_init(&d->fec_lock, NULL);
        pthread_cond_init(&d->fec_cv, NULL);
        d->fec_ok = 1;
        for (int w = 0; ok && w < NFEC; w++) {
            char name[16];
            d->fw[w].d = d;
            ok = !pthread_create(&d->fw[w].th, NULL, fec_thread, &d->fw[w]);
            d->fw[w].started = ok;
            snprintf(name, sizeof(name), "drx-t2fec%d", w);
            if (ok)
                pthread_setname_np(d->fw[w].th, name);
        }
    }
    if (!ok) {
        drx_dvbt2_free(d);
        return NULL;
    }
    make_p1(&d->p1);
    d->state = ST_SEARCH;
    d->sel = -1;
    return d;
}

void drx_dvbt2_free(drx_dvbt2* d)
{
    if (!d)
        return;
    if (d->fec_ok) {
        drx_dvbt2_flush(d);
        pthread_mutex_lock(&d->fec_lock);
        d->fec_stop = 1;
        pthread_cond_broadcast(&d->fec_cv);
        pthread_mutex_unlock(&d->fec_lock);
        for (int w = 0; w < NFEC; w++)
            if (d->fw[w].started)
                pthread_join(d->fw[w].th, NULL);
        pthread_mutex_destroy(&d->fec_lock);
        pthread_cond_destroy(&d->fec_cv);
    }
    for (int w = 0; w < NFEC; w++) {
        struct fecwork* f = &d->fw[w];
        drx_ldpc_free(f->ldpc);
        drx_bch_free(f->bch);
        free(f->bitmap);
        free(f->cellperm);
        free(f->ci);
        free(f->cq);
        free(f->cc);
        free(f->ck);
        free(f->planes);
        free(f->llrsrc);
        free(f->il);
        for (int i = 0; i < 64; i++) {
            free(f->llr[i]);
            free(f->cw[i]);
        }
    }
    for (int i = 0; i < NJOB; i++) {
        free(d->job[i].buf);
        free(d->job[i].csi);
        free(d->job[i].bb);
        free(d->job[i].ok);
        free(d->job[i].err);
        free(d->job[i].sig);
    }
    resamp_free(&d->rs);
    fft_free(&d->fft1k);
    fft_free(&d->fft);
    free_layout(&d->L);
    free(d->p1c1);
    free(d->p1c2);
    free(d->p1c1i);
    free(d->p1c2i);
    free(d->p1e);
    free(d->buf);
    free(d->win);
    free(d->x);
    free(d->hk);
    free(d->hd);
    free(d->hc);
    free(d->h2);
    free(d->grid);
    free(d->hcp);
    free(d->cpp);
    free(d->cells);
    free(d->csi);
    drx_ldpc_free(d->ldpc_pre);
    drx_ldpc_free(d->ldpc_post);
    drx_bch_free(d->bch_pre);
    drx_bch_free(d->bch_post);
    for (int i = 0; i < 64; i++) {
        free(d->llr[i]);
        free(d->cw[i]);
    }
    drx_bbdec_free(d->bb);
    free(d->ts);
    free(d->bbframe);
    free(d);
}

void drx_dvbt2_get_stats(const drx_dvbt2* d, drx_dvbt2_stats* s)
{
    *s = d->st;
    s->fft = d->state == ST_FRAME ? d->n : 0;
    if (d->have_pre) {
        s->guard_num = GI_NUM[d->guard_idx];
        s->guard_den = GI_DEN[d->guard_idx];
        s->extended = d->L.ext;
        s->pilot_pattern = d->L.pp;
        s->num_plp = d->num_plp;
    }
    if (d->sel >= 0) {
        const struct plp* p = &d->plps[d->sel];
        s->plp_id = p->id;
        s->qam = 4 << (2 * p->mod);
        s->rate = PLP_RATES[p->cod & 7];
        s->short_frame = p->fec_type == 0;
        s->rotated = p->rotation;
    } else {
        s->plp_id = -1;
    }
    s->snr_db = 10.0f * log10f((d->mer_sig + 1e-20f) / (d->mer_err + 1e-20f));
    s->freq = (float)(d->f / d->step_nominal);
    s->clock_ppm = (float)(d->clock * 1e6);
}

static void restart(drx_dvbt2* d)
{
    if (d->state != ST_SEARCH)
        d->st.resyncs++;
    d->state = ST_SEARCH;
    d->have_pre = 0;
    d->sel = -1;
}

/* ---- P1 detection and decoding ------------------------------------------------ */

/* DRX_T2_DEBUG=1: P1, guard and L1-pre decoding steps on stderr. */
static int t2_debug(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("DRX_T2_DEBUG");
        v = e && e[0] == '1';
    }
    return v;
}

/* Looks for a P1 in buf[pos..] (at most scan samples on); returns its
 * start, or -1 (pos advanced). */
static int find_p1(drx_dvbt2* d, int scan)
{
    const int avail = d->nbuf - d->pos - P1_LEN - 1;
    if (avail < 4096)
        return -2; /* need more */
    const int ns = avail < scan ? avail : scan;
    const cf* x = d->buf + d->pos;
    /* prefix sums of the shift-compensated lag products and of the energy */
    double complex *c1 = d->p1c1, *c2 = d->p1c2, *c1i = d->p1c1i, *c2i = d->p1c2i;
    double* e = d->p1e;
    c1[0] = c2[0] = c1i[0] = c2i[0] = 0;
    e[0] = 0;
    const int m = ns + P1_LEN;
    for (int i = 0; i < m; i++) {
        const cf r = d->p1.rot[i & 1023];
        const cf p1 = i + P1_C < d->nbuf - d->pos ? x[i] * conjf(x[i + P1_C]) : 0;
        const cf p2 = i >= P1_B ? x[i] * conjf(x[i - P1_B]) : 0;
        c1[i + 1] = c1[i] + p1 * r;
        c2[i + 1] = c2[i] + p2 * r;
        /* inverted spectrum: the frequency shifts of the C and B parts
         * turn the other way */
        c1i[i + 1] = c1i[i] + p1 * conjf(r);
        c2i[i + 1] = c2i[i] + p2 * conjf(r);
        e[i + 1] = e[i] + crealf(x[i] * conjf(x[i]));
    }
    int best = -1, best_inv = 0;
    float bv = P1_MIN;
    for (int s = 0; s < ns; s++) {
        const double en = (e[s + P1_LEN] - e[s]) * (double)(P1_C + P1_B) / P1_LEN;
        for (int iv = 0; iv < 2; iv++) {
            const double complex* a = iv ? c1i : c1;
            const double complex* b = iv ? c2i : c2;
            const double complex S1 = a[s + P1_C] - a[s];
            const double complex S2 = b[s + P1_LEN] - b[s + 1566];
            const double a1 = sqrt(creal(S1) * creal(S1) + cimag(S1) * cimag(S1));
            const double a2 = sqrt(creal(S2) * creal(S2) + cimag(S2) * cimag(S2));
            const float v = (float)((a1 + a2) / (en + 1e-30));
            if (v > bv) {
                bv = v;
                best = s;
                best_inv = iv;
            }
        }
        if (best >= 0 && s > best + 1024)
            break;
    }
    if (best >= 0 && best_inv) {
        /* the P1 shows on the conjugated input: conjugate what is buffered
         * and what comes next, then look again (now the normal way) */
        for (int i = d->pos; i < d->nbuf; i++)
            d->buf[i] = conjf(d->buf[i]);
        d->inv ^= 1;
        if (t2_debug())
            fprintf(stderr, "t2: spectrum %s\n", d->inv ? "inverted" : "normal");
        return find_p1(d, scan);
    }
    if (best >= 0) {
        const double complex S1 = c1[best + P1_C] - c1[best];
        const double complex S2 = c2[best + P1_LEN] - c2[best + 1566];
        /* S1 turns by -2 pi f 542, S2 by +2 pi f 482 */
        d->f = carg(S2 * conj(S1)) / (2 * M_PI * 1024);
    }
    if (best < 0) {
        d->pos += ns;
        return -1;
    }
    return d->pos + best;
}

/* FFT of the main part of the P1 at s: integer offset, S1, S2. */
static int decode_p1(drx_dvbt2* d, int s)
{
    cf a[1024];
    const double w = -2 * M_PI * d->f;
    for (int i = 0; i < 1024; i++)
        a[i] = d->buf[s + P1_C + i] * cexpf(I * (float)fmod(w * (s + P1_C + i), 2 * M_PI));
    fft_run(&d->fft1k, a, 0);
    float best = -1;
    int bi = 0;
    for (int off = -P1_IMAX; off <= P1_IMAX; off++) {
        float p = 0;
        for (int i = 0; i < 384; i++) {
            const cf v = a[(T2_P1_CARRIERS[i] - 426 + off + 2048) & 1023];
            p += crealf(v * conjf(v));
        }
        if (p > best) {
            best = p;
            bi = off;
        }
    }
    d->f += bi / 1024.0;
    /* descrambled carriers, then the sign changes between neighbours */
    cf c[384];
    for (int i = 0; i < 384; i++)
        c[i] = a[(T2_P1_CARRIERS[i] - 426 + bi + 2048) & 1023] * d->p1.scr[i];
    float dd[384];
    dd[0] = 0;
    for (int i = 1; i < 384; i++)
        dd[i] = crealf(c[i] * conjf(c[i - 1]));
    /* DBPSK: element i flips when the sequence bit is 1; correlate the
     * flips with each pattern (the first element of each part is
     * compared with the last of the previous part) */
    float b1 = -1e30f, b2 = -1e30f;
    int v1 = 0, v2 = 0;
    for (int v = 0; v < 8; v++) {
        float sc = 0;
        for (int i = 1; i < 64; i++)
            sc += dd[i] * d->p1.s1[v][i];
        for (int i = 0; i < 64; i++)
            sc += dd[320 + i] * d->p1.s1[v][i];
        if (sc > b1) {
            b1 = sc;
            v1 = v;
        }
    }
    for (int v = 0; v < 16; v++) {
        float sc = 0;
        for (int i = 0; i < 256; i++)
            sc += dd[64 + i] * d->p1.s2[v][i];
        if (sc > b2) {
            b2 = sc;
            v2 = v;
        }
    }
    d->s1 = v1;
    d->s2 = v2;
    if (t2_debug())
        fprintf(stderr, "t2: P1 at %d, S1 %d S2 %d, offset %.5f\n", s, v1, v2, d->f);
    if (v1 != 0) /* T2 SISO only */
        return -1;
    static const int FFT_OF_S2[8] = { 2048, 8192, 4096, 1024, 16384, 32768, 8192, 32768 };
    d->n = FFT_OF_S2[v2 >> 1];
    return 0;
}

/* Guard interval by cyclic prefix correlation over the first symbols after
 * the P1 at s. */
static int detect_guard(drx_dvbt2* d, int s)
{
    const int n = d->n;
    float best = 0;
    int bg = -1;
    cf bc = 0;
    for (int gi = 0; gi < 7; gi++) {
        const int g = n * GI_NUM[gi] / GI_DEN[gi];
        if (s + P1_LEN + 4 * (n + g) + n > d->nbuf)
            return -2;
        float sc = 0;
        cf call = 0;
        for (int l = 0; l < 4; l++) {
            const cf* x = d->buf + s + P1_LEN + l * (n + g);
            cf c = 0;
            float e = 0;
            for (int t = 0; t < g; t++) {
                c += x[t] * conjf(x[t + n]);
                e += 0.5f * (crealf(x[t] * conjf(x[t])) + crealf(x[t + n] * conjf(x[t + n])));
            }
            sc += cabsf(c) / (e + 1e-30f);
            call += c;
        }
        if (sc > best) {
            best = sc;
            bg = gi;
            bc = call;
        }
    }
    if (best < 4 * 0.3f)
        return -1;
    d->guard_idx = bg;
    d->g = n * GI_NUM[bg] / GI_DEN[bg];
    d->sym_len = n + d->g;
    /* fine carrier offset: the guard interval correlation turns by
     * -2 pi f n, which pins f to within 1/(2n) of the P1 estimate (a 32K
     * carrier is only 1/32768 cycle per sample wide) */
    const double fcp = -cargf(bc) / (2 * M_PI * n);
    double df = fcp - d->f;
    df -= floor(df * n + 0.5) / n;
    d->f += df;
    return 0;
}

/* ---- OFDM symbols ------------------------------------------------------------ */

/* FFT of the symbol starting (guard interval included) at pos: carriers
 * 0..kmax of a layout with ktot carriers into d->x. */
static void demod_symbol(drx_dvbt2* d, int ktot)
{
    const int n = d->n;
    /* window: a quarter of the guard interval before the useful part */
    const int start = d->pos + d->g - d->g / 4;
    const double w = -2 * M_PI * d->f;
    /* carrier offset rotation: exact every 256 samples, in between advanced
     * 4 samples at a time on 4 lanes (rotations r st^0..3, times st^4) */
    const cf st = cexpf(I * (float)w), st2 = st * st;
    const cf pw[4] = { 1, st, st2, st2 * st };
    const cf st4[4] = { st2 * st2, st2 * st2, st2 * st2, st2 * st2 };
    const v8f vst4 = ldf((const float*)st4);
    for (int i = 0; i < n; i += 256) {
        const cf r = cexpf(I * (float)fmod(d->phase + w * (d->g - d->g / 4 + i), 2 * M_PI));
        cf r4[4];
        for (int j = 0; j < 4; j++)
            r4[j] = r * pw[j];
        v8f rr = ldf((const float*)r4);
        /* n is a multiple of 256 */
        for (int j = i; j < i + 256; j += 4) {
            stf((float*)(d->win + j), cmul(ldf((const float*)(d->buf + start + j)), rr));
            rr = cmul(rr, vst4);
        }
    }
    fft_run(&d->fft, d->win, 0);
    /* carriers -kc..kc - 1 (n is a power of two) */
    const float scale = 1.0f / sqrtf((float)n);
    const int kc = (ktot - 1) / 2;
    for (int k = 0; k < kc; k++)
        d->x[k] = d->win[n - kc + k] * scale;
    for (int k = kc; k < ktot; k++)
        d->x[k] = d->win[k - kc] * scale;
}

static void advance(drx_dvbt2* d, int samples)
{
    d->pos += samples;
    d->phase = fmod(d->phase - 2 * M_PI * d->f * samples, 2 * M_PI);
}

static int pn_bit(int l)
{
    return (T2_PN_SEQUENCE[l / 8] >> (7 - l % 8)) & 1;
}

/* Linear interpolation of a channel known on every step-th carrier (all
 * of hk[0], hk[step], ...), with the mean phase slope taken out. */
static void interp_dense(cf* hk, int kmax, int step)
{
    cf ph = 0;
    for (int k = step; k <= kmax; k += step)
        ph += hk[k] * conjf(hk[k - step]);
    const float phi = cargf(ph);
    const cf back = cexpf(-I * phi);
    cf rot[6]; /* step is 3 or 6 */
    for (int r = 1; r < step && r < 6; r++)
        rot[r] = cexpf(I * phi * ((float)r / step));
    for (int k0 = 0; k0 + step <= kmax; k0 += step) {
        const cf a = hk[k0], b = hk[k0 + step] * back;
        for (int r = 1; r < step; r++) {
            const float t = (float)r / step;
            hk[k0 + r] = (a * (1 - t) + b * t) * rot[r];
        }
    }
    /* beyond the last pilot (kmax not on the grid) */
    const int last = kmax / step * step;
    for (int k = last + 1; k <= kmax; k++)
        hk[k] = hk[last];
}

static void ensure_cells(drx_dvbt2* d)
{
    const struct layout* L = &d->L;
    const int need = L->np2 * L->cp2 + (d->ldata + 1) * L->cdata + L->nfc + 16;
    if (need <= d->ncells_cap)
        return;
    cf* c = realloc(d->cells, sizeof(cf) * need);
    if (c)
        d->cells = c;
    float* h = realloc(d->csi, sizeof(float) * need);
    if (h)
        d->csi = h;
    d->ncells_cap = c && h ? need : d->ncells_cap;
}

/* Start of the cells of symbol l in the frame buffer. */
static int cell_base(const drx_dvbt2* d, int l)
{
    const struct layout* L = &d->L;
    return l < L->np2 ? l * L->cp2 : L->np2 * L->cp2 + (l - L->np2) * L->cdata;
}

/* Equalised data cells of the current symbol into the frame buffer: the
 * channel h of every carrier (P2: given; data symbols: interpolated between
 * the grid points, the phase slope taken out), then x conj(h) / |h|^2 and
 * |h|^2 for every carrier, 4 at a time; the data carriers in a row, then
 * the cells in order, each from its data carrier (frequency deinterleaving
 * by the inverse permutation: the frame buffer is written in sequence, the
 * random reads are from the symbol's carriers, in cache). */
static void store_cells(drx_dvbt2* d, const int* carriers, int nc, const int* hie, const int* hio,
                        const cf* h, int base, int grid)
{
    const int* Hi = d->l & 1 ? hio : hie;
    const struct layout* L = &d->L;
    if (grid) {
        cf* hd = d->hd;
        const int dx = L->dx;
        float tf[24];
        for (int r = 1; r < dx; r++)
            tf[r] = (float)r / dx;
        for (int i = 0; i < L->ngrid; i++) {
            const int k0 = i * dx;
            const cf a = d->grid[i];
            hd[k0] = a;
            if (i + 1 >= L->ngrid) {
                for (int k = k0 + 1; k <= L->kmax; k++)
                    hd[k] = a;
                break;
            }
            const cf b = d->grid[i + 1] * d->slope_back;
            for (int r = 1; r < dx; r++) {
                const float t = tf[r];
                hd[k0 + r] = (a * (1 - t) + b * t) * d->slope_tab[r];
            }
        }
        h = hd;
    }
    cf* x = d->x;
    float* h2 = d->h2;
    const v8f eps = vsplat(1e-12f);
    for (int k = 0; k <= L->kmax; k += 4) {
        const v8f hv = ldf((const float*)(h + k));
        const v8f p = hv * hv;
        const v8f m = p + swap_ri(p); /* |h|^2 in both halves */
        stf(h2 + 2 * k, m);
        stf((float*)(x + k), cmulc(ldf((const float*)(x + k)), hv) / (m + eps));
    }
    cf* const xd = d->hd;
    float* const hc = d->hc;
    for (int j = 0; j < nc; j++) {
        const int k = carriers[j];
        xd[j] = x[k];
        hc[j] = h2[2 * k];
    }
    const int lim = nc < d->ncells_cap - base ? nc : d->ncells_cap - base;
    cf* cells = d->cells + base;
    float* csi = d->csi + base;
    for (int q = 0; q < lim; q++) {
        const int j = Hi[q];
        cells[q] = xd[j];
        csi[q] = hc[j];
    }
}

/* The data symbol estimates start from the channel of the last P2 symbol
 * (again once the L1 gives the pilot pattern). */
static void seed_from_p2(drx_dvbt2* d)
{
    const struct layout* L = &d->L;
    for (int j = 0; j < L->ngrid; j++)
        d->grid[j] = d->hk[j * L->dx];
    for (int i = 0; i < L->ncp; i++)
        d->hcp[i] = d->hk[L->cp[i]];
    d->have_cpp = 0;
}

/* P2 symbol: channel from the P2 pilots. */
static void p2_symbol(drx_dvbt2* d)
{
    const struct layout* L = &d->L;
    const int step = d->n == 32768 ? 6 : 3;
    const float sgn = pn_bit(d->l) ? -1.0f : 1.0f;
    for (int k = 0; k <= L->kmax; k += step)
        d->hk[k] = d->x[k] / (L->ap2 * L->w[k] * sgn);
    interp_dense(d->hk, L->kmax, step);
    store_cells(d, L->p2_data, L->cp2, L->hp2e, L->hp2o, d->hk, cell_base(d, d->l), 0);
    seed_from_p2(d);
}

/* Rotates the channel references as a timing change of t samples does. */
static void shift_refs(drx_dvbt2* d, double t)
{
    const struct layout* L = &d->L;
    const double w = -2 * M_PI * t / d->n;
    /* grid: a rotation advanced by e^(j w Dx) (in double, exact every 64) */
    const double complex st = cexp(I * (w * L->dx));
    double complex r = 0;
    for (int j = 0; j < L->ngrid; j++) {
        if ((j & 63) == 0)
            r = cexp(I * (w * (j * L->dx - L->kc)));
        d->grid[j] *= (cf)r;
        r *= st;
    }
    for (int i = 0; i < L->ncp; i++) {
        const cf r = cexpf(I * (float)(w * (L->cp[i] - L->kc)));
        d->hcp[i] *= r;
        d->cpp[i] *= r;
    }
}

/* Data or frame closing symbol. */
static void data_symbol(drx_dvbt2* d, int closing)
{
    const struct layout* L = &d->L;
    const float sgn = pn_bit(d->l) ? -1.0f : 1.0f;
    shift_refs(d, d->drift);

    /* common phase error on the continual pilots (amplitude ASP where they
     * are scattered pilots too) */
    const int p = (d->l) % L->dy;
    cf s = 0;
    if (!closing) {
        for (int i = 0; i < L->ncp; i++) {
            const int k = L->cp[i];
            const float a = L->cp_res[i] == L->dx * p ? L->asp : L->acp;
            s += d->x[k] / (a * L->w[k] * sgn) * conjf(d->hcp[i]);
        }
        const float psi = cargf(s);
        const cf derot = cexpf(-I * psi);
        for (int k = 0; k <= L->kmax; k++)
            d->x[k] *= derot;
        d->f += (d->tracked < CLOCK_ACQ ? KF_ACQ : KF) * psi / (2 * M_PI * d->sym_len);
        /* clock: phase change of the continual pilots across the band */
        cf lo = 0, hi = 0;
        double klo = 0, khi = 0;
        int nlo = 0, nhi = 0;
        for (int i = 0; i < L->ncp; i++) {
            const int k = L->cp[i];
            const float a = L->cp_res[i] == L->dx * p ? L->asp : L->acp;
            const cf h = d->x[k] / (a * L->w[k] * sgn);
            if (d->have_cpp) {
                const cf c = h * conjf(d->cpp[i]);
                if (k < L->kc) {
                    lo += c;
                    klo += k - L->kc;
                    nlo++;
                } else {
                    hi += c;
                    khi += k - L->kc;
                    nhi++;
                }
            }
            d->cpp[i] = h;
            d->hcp[i] = h;
        }
        if (d->have_cpp && nlo && nhi) {
            const double slope = cargf(hi * conjf(lo)) / (khi / nhi - klo / nlo);
            const double res = -slope * d->n / (2 * M_PI);
            d->tau += d->drift + res;
            d->drift += (d->tracked < CLOCK_ACQ ? 0.5 : 0.05) * res;
            const double target = d->cur_step * (1 + d->drift / d->sym_len);
            d->clock = target / d->step_nominal - 1;
            if (d->tracked >= CLOCK_ACQ && fabs(d->clock) < 200e-6)
                d->step = target;
        }
        d->have_cpp = 1;
        d->tracked++;
    }

    /* channel grid: scattered pilots of this symbol, edges (closing
     * symbol: every grid point) */
    const int D = L->dx * L->dy;
    int r = -L->kext % D; /* (k - Kext) mod Dx Dy */
    if (r < 0)
        r += D;
    for (int j = 0; j < L->ngrid; j++, r = r + L->dx >= D ? r + L->dx - D : r + L->dx) {
        const int k = j * L->dx;
        if (closing || k == 0 || k == L->kmax || r == L->dx * p)
            d->grid[j] = d->x[k] / (L->asp * L->w[k] * sgn);
    }
    cf ph = 0;
    for (int j = 1; j < L->ngrid; j++)
        ph += d->grid[j] * conjf(d->grid[j - 1]);
    const float phi = cargf(ph);
    d->slope_back = cexpf(-I * phi);
    for (int r = 0; r < L->dx; r++)
        d->slope_tab[r] = cexpf(I * phi * r / L->dx);

    if (closing)
        store_cells(d, L->fc_data, L->nfc, L->hfce, L->hfco, NULL, cell_base(d, d->l), 1);
    else
        store_cells(d, L->data[p], L->cdata, L->hde, L->hdo, NULL, cell_base(d, d->l), 1);
}

/* ---- L1 ------------------------------------------------------------------------ */

static uint32_t crc32_bits(const uint8_t* b, int n)
{
    uint32_t c = 0xFFFFFFFF;
    for (int i = 0; i < n; i++) {
        const uint32_t top = (c >> 31) ^ b[i];
        c <<= 1;
        if (top)
            c ^= 0x04C11DB7;
    }
    return c;
}

/* n bits (fields of more than 31 bits are only skipped: unsigned shifts) */
static int bits(const uint8_t* b, int* at, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        v = v << 1 | b[(*at)++];
    return (int)v;
}

static inline int8_t to_llr(float v)
{
    return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : lrintf(v));
}

static inline v8f vand(v8f a, v8i m)
{
    return (v8f)((v8i)a & m);
}

static inline v8f vabs(v8f a)
{
    return vand(a, (v8i){ 0, 0, 0, 0, 0, 0, 0, 0 } + INT32_MAX);
}

#ifdef __AVX2__
static inline v8f vmin(v8f a, v8f b)
{
    return (v8f)_mm256_min_ps((__m256)a, (__m256)b);
}

static inline v8f vmax(v8f a, v8f b)
{
    return (v8f)_mm256_max_ps((__m256)a, (__m256)b);
}
#else
/* (C has no vector ?:) */
static inline v8f vmin(v8f a, v8f b)
{
    const v8i m = a < b;
    return (v8f)(((v8i)a & m) | ((v8i)b & ~m));
}

static inline v8f vmax(v8f a, v8f b)
{
    const v8i m = a > b;
    return (v8f)(((v8i)a & m) | ((v8i)b & ~m));
}
#endif

/* toward zero */
static inline v8f vtrunc(v8f a)
{
    return __builtin_convertvector(__builtin_convertvector(a, v8i), v8f);
}

#ifdef __AVX2__
/* to_llr of 32 values (4 vectors), in order in a vector of bytes: clamped,
 * rounded to nearest even (as lrintf), packed */
static inline __m256i to_llr32v(const v8f* v)
{
    const v8f hi = vsplat(127), lo = vsplat(-127);
    __m256i w[4];
    for (int i = 0; i < 4; i++)
        w[i] = _mm256_cvtps_epi32((__m256)vmax(vmin(v[i], hi), lo));
    /* the packs work per 128-bit half: bytes come as 0-3 8-11 16-19 24-27
     * 4-7 12-15 20-23 28-31 (groups of 4) */
    const __m256i b = _mm256_packs_epi16(_mm256_packs_epi32(w[0], w[1]), _mm256_packs_epi32(w[2], w[3]));
    return _mm256_permutevar8x32_epi32(b, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
}
#endif

/* to_llr of 32 values to p[0..31] */
static inline void to_llr32(int8_t* p, const v8f* v)
{
#ifdef __AVX2__
    _mm256_storeu_si256((__m256i*)p, to_llr32v(v));
#else
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < 8; l++)
            p[8 * i + l] = to_llr(v[i][l]);
#endif
}

/* The same, values 0, 2 .. 30 to pe[0..15], 1, 3 .. 31 to po[0..15]. */
static inline void to_llr32_split(int8_t* pe, int8_t* po, const v8f* v)
{
#ifdef __AVX2__
    const __m256i eo = _mm256_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15, 0, 2, 4, 6, 8, 10,
                                        12, 14, 1, 3, 5, 7, 9, 11, 13, 15);
    /* per half: 8 even, 8 odd; then the quarters 0 2 1 3 */
    const __m256i b = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(to_llr32v(v), eo), 0xD8);
    _mm_storeu_si128((__m128i*)pe, _mm256_castsi256_si128(b));
    _mm_storeu_si128((__m128i*)po, _mm256_extracti128_si256(b, 1));
#else
    for (int l = 0; l < 32; l++)
        (l & 1 ? po : pe)[l >> 1] = to_llr(v[l / 8][l % 8]);
#endif
}

/* LDPC + BCH of a 16K L1 codeword; info gets kbch bits. */
static int l1_fec(drx_ldpc* ldpc, drx_bch* bch, const drx_fec_params* p, int8_t* llr, uint8_t* info)
{
    uint8_t* cw = malloc(p->nldpc / 8 + 1);
    if (!cw)
        return -1;
    const int8_t* l[1] = { llr };
    uint8_t* o[1] = { cw };
    drx_ldpc_decode(ldpc, l, o, 1, LDPC_ITER);
    const int r = drx_bch_decode(bch, cw);
    for (int i = 0; i < p->kbch; i++)
        info[i] = (cw[i / 8] >> (7 - i % 8)) & 1;
    free(cw);
    return r < 0 ? -1 : 0;
}

/* L1 cell of index i of a part starting at offset off (cells per P2
 * symbol for that part: per), across the P2 symbols. */
static int l1_cell(const drx_dvbt2* d, int off, int i)
{
    const struct layout* L = &d->L;
    return (i % L->np2) * L->cp2 + off / L->np2 + i / L->np2;
}

static float noise_from_bpsk(const drx_dvbt2* d, int n)
{
    float e = 0;
    for (int i = 0; i < n; i++) {
        const int q = l1_cell(d, 0, i);
        const cf z = d->cells[q];
        const float r = crealf(z) >= 0 ? 1.0f : -1.0f;
        e += ((crealf(z) - r) * (crealf(z) - r) + cimagf(z) * cimagf(z)) * d->csi[q];
    }
    return e / n + 1e-6f;
}

static int decode_l1pre(drx_dvbt2* d)
{
    const drx_fec_params* p = &d->fec_pre;
    int8_t* llr = d->llr[0];
    uint8_t info[3072];
    const float n0 = noise_from_bpsk(d, 1840);
    memset(llr, 0, p->nldpc);
    /* 200 information bits, zero padding to 3072, 168 BCH parity, then the
     * LDPC parity bits left by the puncturing (clause 7.3.2) */
    for (int i = 200; i < 3072; i++)
        llr[i] = 127;
    uint8_t* punct = calloc(p->nldpc - p->kldpc, 1);
    if (!punct)
        return -1;
    for (int c = 0; c < 31; c++)
        for (int s = 0; s < 360; s++)
            punct[s * 36 + T2_PRE_PUNCTURE[c]] = 1;
    for (int s = 0; s < 11488 - 31 * 360; s++)
        punct[s * 36 + T2_PRE_PUNCTURE[31]] = 1;
    int c = 0;
    const float k = 8.0f / n0;
    for (int i = 0; i < 200; i++, c++) {
        const int q = l1_cell(d, 0, c);
        llr[i] = to_llr(crealf(d->cells[q]) * d->csi[q] * k);
    }
    for (int i = 3072; i < 3240; i++, c++) {
        const int q = l1_cell(d, 0, c);
        llr[i] = to_llr(crealf(d->cells[q]) * d->csi[q] * k);
    }
    for (int i = 0; i < p->nldpc - p->kldpc; i++)
        if (!punct[i]) {
            const int q = l1_cell(d, 0, c++);
            llr[p->kldpc + i] = to_llr(crealf(d->cells[q]) * d->csi[q] * k);
        }
    free(punct);
    if (c != 1840 || l1_fec(d->ldpc_pre, d->bch_pre, p, llr, info)) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-pre FEC failed (%d cells)\n", c);
        return -1;
    }
    if (crc32_bits(info, 200) != 0) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-pre CRC failed\n");
        return -1;
    }
    struct l1pre* e = &d->pre;
    int at = 0;
    e->type = bits(info, &at, 8);
    e->bwt_ext = bits(info, &at, 1);
    e->s1 = bits(info, &at, 3);
    e->s2 = bits(info, &at, 4);
    e->repetition = bits(info, &at, 1);
    e->guard = bits(info, &at, 3);
    e->papr = bits(info, &at, 4);
    e->l1_mod = bits(info, &at, 4);
    e->l1_cod = bits(info, &at, 2);
    e->l1_fec_type = bits(info, &at, 2);
    e->post_size = bits(info, &at, 18);
    e->post_info_size = bits(info, &at, 18);
    e->pilot_pattern = bits(info, &at, 4) + 1;
    e->tx_id = bits(info, &at, 8);
    e->cell_id = bits(info, &at, 16);
    e->network_id = bits(info, &at, 16);
    e->t2_system_id = bits(info, &at, 16);
    e->num_t2_frames = bits(info, &at, 8);
    e->num_data_symbols = bits(info, &at, 12);
    e->regen = bits(info, &at, 3);
    e->post_extension = bits(info, &at, 1);
    e->num_rf = bits(info, &at, 3);
    e->current_rf = bits(info, &at, 3);
    e->t2_version = bits(info, &at, 4);
    e->post_scrambled = bits(info, &at, 1);
    e->base_lite = bits(info, &at, 1);
    if (t2_debug())
        fprintf(stderr, "t2: L1-pre type %d bwt_ext %d S1 %d S2 %d guard %d papr %d l1 mod %d cod %d "
                "PP%d frames %d symbols %d version %d post_scr %d\n", e->type, e->bwt_ext, e->s1, e->s2,
                e->guard, e->papr, e->l1_mod, e->l1_cod, e->pilot_pattern, e->num_t2_frames,
                e->num_data_symbols, e->t2_version, e->post_scrambled);
    if (e->pilot_pattern > 8 || e->guard > 6 || e->l1_mod > 3 || e->num_data_symbols < 1)
        return -1;
    return 0;
}

/* Demaps an L1 cell (non-rotated) into eta soft values (d1 - d0) / 4, in
 * the level units of the constellation. */
static void l1_demap(int eta, cf z, float* out)
{
    if (eta == 1) {
        out[0] = crealf(z);
        return;
    }
    const float norm = eta == 2 ? (float)M_SQRT2 : eta == 4 ? sqrtf(10) : sqrtf(42);
    const float re = crealf(z) * norm, im = cimagf(z) * norm;
    const int nb = eta / 2;
    for (int a = 0; a < 2; a++) {
        const float r = a ? im : re;
        out[a] = r;
        if (nb >= 2) {
            const float u = fabsf(r), t = nb == 2 ? 2.0f : 4.0f;
            out[a + 2] = u - t;
            if (nb == 3)
                out[a + 4] = fabsf(u - 4) - 2;
        }
    }
}

static int decode_l1post(drx_dvbt2* d)
{
    const struct l1pre* e = &d->pre;
    const drx_fec_params* p = &d->fec_post;
    static const int ETA[4] = { 1, 2, 4, 6 };
    const int eta = ETA[e->l1_mod];
    const int kpost_ex = e->post_info_size + 32;
    const int nblocks = (kpost_ex + p->kbch - 1) / p->kbch;
    const int ksig = (kpost_ex + nblocks - 1) / nblocks;
    const int kpost = ksig * nblocks;
    if (e->post_size * eta % nblocks) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-post size %d x %d not divisible in %d blocks\n", e->post_size, eta, nblocks);
        return -1;
    }
    const int npost = e->post_size * eta / nblocks;
    const int npunc = ksig + 168 + 9000 - npost;
    if (npunc < 0 || npunc > 9000 || ksig > p->kbch) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-post npunc %d ksig %d\n", npunc, ksig);
        return -1;
    }
    const uint8_t* padding = eta <= 2 ? T2_POST_PADDING_BQPSK : eta == 4 ? T2_POST_PADDING_16QAM : T2_POST_PADDING_64QAM;
    const uint8_t* puncture = eta <= 2 ? T2_POST_PUNCTURE_BQPSK : eta == 4 ? T2_POST_PUNCTURE_16QAM : T2_POST_PUNCTURE_64QAM;
    /* information positions: groups of 360 bits (the last of 192) */
    uint8_t* padded = calloc(p->kbch, 1);
    uint8_t* punct = calloc(9000, 1);
    float* soft = malloc(sizeof(float) * npost);
    uint8_t* info = malloc(p->kbch);
    uint8_t* post = malloc(kpost);
    if (!padded || !punct || !soft || !info || !post) {
        free(padded);
        free(punct);
        free(soft);
        free(info);
        free(post);
        return -1;
    }
    const int ngroup = 20;
    const int npad = ksig <= 360 ? ngroup - 1 : (p->kbch - ksig) / 360;
    for (int i = 0; i < npad; i++) {
        const int g = padding[i], len = g == 19 ? 192 : 360;
        memset(padded + g * 360, 1, len);
    }
    {
        const int g = padding[npad], len = g == 19 ? 192 : 360;
        const int extra = npad == ngroup - 1 ? 360 - ksig : p->kbch - ksig - 360 * npad;
        if (extra > 0)
            memset(padded + g * 360 + len - extra, 1, extra);
    }
    for (int c = 0; c < npunc / 360; c++)
        for (int s = 0; s < 360; s++)
            punct[s * 25 + puncture[c]] = 1;
    for (int s = 0; s < npunc % 360; s++)
        punct[s * 25 + puncture[npunc / 360]] = 1;

    int ok = 1, cell = 0;
    for (int b = 0; b < nblocks && ok; b++) {
        /* noise (complex, level units) from the hard decisions */
        const float norm = eta == 1 ? 1.0f : eta == 2 ? (float)M_SQRT2 : eta == 4 ? sqrtf(10) : sqrtf(42);
        const int na = eta == 1 ? 1 : 1 << (eta / 2 - 1);
        float err = 0;
        for (int i = 0; i < npost / eta; i++) {
            const int q = l1_cell(d, 1840, cell + i);
            const cf z = d->cells[q];
            const float re = fabsf(crealf(z)) * norm, im = fabsf(cimagf(z)) * norm;
            float lr = 2 * floorf(re * 0.5f) + 1, li = 2 * floorf(im * 0.5f) + 1;
            lr = lr > 2 * na - 1 ? 2 * na - 1 : lr;
            li = eta == 1 ? 0 : li > 2 * na - 1 ? 2 * na - 1 : li;
            err += ((re - lr) * (re - lr) + (im - li) * (im - li)) * d->csi[q];
        }
        const float n0u = err / (npost / eta) + 1e-6f;
        if (t2_debug())
            fprintf(stderr, "t2: L1-post block %d: MER %.1f dB over %d cells\n", b,
                    10 * log10f((eta == 1 ? 1.0f : 2.0f * (4.0f * (1 << (eta - 2)) - 1) / 3.0f) / n0u),
                    npost / eta);
        /* demultiplexer and bit interleaver (16/64-QAM): soft[j] is bit j of
         * the codeword in transmission order */
        const int nc = eta == 4 ? 8 : 12, nr = npost / nc;
        for (int i = 0; i < npost / eta; i++) {
            const int q = l1_cell(d, 1840, cell + i);
            float s6[6];
            l1_demap(eta, d->cells[q], s6);
            for (int t = 0; t < eta; t++)
                s6[t] *= d->csi[q] * 8.0f / n0u;
            if (eta <= 2) {
                for (int t = 0; t < eta; t++)
                    soft[i * eta + t] = s6[t];
                continue;
            }
            /* cell i is half of row i/2: bit position of the pair */
            const int row = i / 2, half = i & 1;
            const int* mux = eta == 4 ? MUX16 : MUX64;
            for (int e2 = 0; e2 < nc; e2++) {
                const int pos = mux[e2];
                if ((pos >= eta) != half)
                    continue;
                /* row-wise read: element (row, e2) was written column-wise */
                soft[e2 * nr + row] = s6[pos - half * eta];
            }
        }
        cell += npost / eta;
        int8_t* llr = d->llr[0];
        memset(llr, 0, p->nldpc);
        int j = 0;
        for (int i = 0; i < p->kbch; i++)
            llr[i] = padded[i] ? 127 : to_llr(soft[j++]);
        for (int i = p->kbch; i < p->kldpc; i++)
            llr[i] = to_llr(soft[j++]);
        for (int i = 0; i < 9000; i++)
            if (!punct[i])
                llr[p->kldpc + i] = to_llr(soft[j++]);
        if (j != npost || l1_fec(d->ldpc_post, d->bch_post, p, llr, info)) {
            ok = 0;
            break;
        }
        int o = 0;
        for (int i = 0; i < p->kbch; i++)
            if (!padded[i])
                post[b * ksig + o++] = info[i];
    }
    free(padded);
    free(punct);
    free(soft);
    free(info);
    if (!ok) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-post FEC failed (%d blocks, %d cells/block)\n", nblocks, npost);
        free(post);
        return -1;
    }
    if (e->post_scrambled) {
        uint8_t* s = malloc(ksig);
        if (s) {
            for (int b = 0; b < nblocks; b++) {
                prbs15(0x4A80, ksig, s);
                for (int i = 0; i < ksig; i++)
                    post[b * ksig + i] ^= s[i];
            }
            free(s);
        }
    }
    if (crc32_bits(post, kpost_ex) != 0) {
        if (t2_debug())
            fprintf(stderr, "t2: L1-post CRC failed\n");
        free(post);
        return -1;
    }
    /* configurable */
    int at = 0;
    bits(post, &at, 15); /* SUB_SLICES_PER_FRAME */
    const int num_plp = bits(post, &at, 8);
    const int num_aux = bits(post, &at, 4);
    bits(post, &at, 8);
    for (int i = 0; i < e->num_rf; i++)
        bits(post, &at, 3 + 32);
    if (e->s2 & 1)
        bits(post, &at, 4 + 22 + 8);
    if (num_plp > MAX_PLP || num_plp < 1) {
        if (t2_debug())
            fprintf(stderr, "t2: %d PLPs\n", num_plp);
        free(post);
        return -1;
    }
    for (int i = 0; i < num_plp; i++) {
        struct plp* q = &d->plps[i];
        q->id = bits(post, &at, 8);
        q->type = bits(post, &at, 3);
        q->payload_type = bits(post, &at, 5);
        q->ff = bits(post, &at, 1);
        q->first_rf = bits(post, &at, 3);
        q->first_frame = bits(post, &at, 8);
        q->group = bits(post, &at, 8);
        q->cod = bits(post, &at, 3);
        q->mod = bits(post, &at, 3);
        q->rotation = bits(post, &at, 1);
        q->fec_type = bits(post, &at, 2);
        q->num_blocks_max = bits(post, &at, 10);
        q->frame_interval = bits(post, &at, 8);
        q->time_il_length = bits(post, &at, 8);
        q->time_il_type = bits(post, &at, 1);
        q->in_band_a = bits(post, &at, 1);
        q->in_band_b = bits(post, &at, 1);
        bits(post, &at, 11);
        q->mode = bits(post, &at, 2);
        q->static_flag = bits(post, &at, 1);
        q->static_padding = bits(post, &at, 1);
    }
    bits(post, &at, 2 + 30);
    for (int i = 0; i < num_aux; i++)
        bits(post, &at, 4 + 28);
    /* dynamic */
    bits(post, &at, 8 + 22 + 22 + 8 + 3 + 8);
    for (int i = 0; i < num_plp; i++) {
        const int id = bits(post, &at, 8);
        const int start = bits(post, &at, 22);
        const int nb = bits(post, &at, 10);
        bits(post, &at, 8);
        for (int k = 0; k < num_plp; k++)
            if (d->plps[k].id == id) {
                d->plps[k].start = start;
                d->plps[k].num_blocks = nb;
            }
    }
    free(post);
    d->num_plp = num_plp;
    /* the PLP to decode: asked for, or the first data PLP */
    d->sel = -1;
    if (t2_debug())
        for (int i = 0; i < num_plp; i++)
            fprintf(stderr, "t2: PLP %d type %d payload %d cod %d mod %d rot %d fec %d blocks %d ti %d/%d mode %d\n",
                    d->plps[i].id, d->plps[i].type, d->plps[i].payload_type, d->plps[i].cod, d->plps[i].mod,
                    d->plps[i].rotation, d->plps[i].fec_type, d->plps[i].num_blocks, d->plps[i].time_il_type,
                    d->plps[i].time_il_length, d->plps[i].mode);
    for (int i = 0; i < num_plp; i++) {
        const struct plp* q = &d->plps[i];
        if (d->want_plp >= 0 ? q->id == d->want_plp : q->type == 1 || q->type == 2) {
            d->sel = i;
            break;
        }
    }
    return 0;
}

/* After the P2 symbols: L1, then the layout of the data symbols. */
static int frame_l1(drx_dvbt2* d)
{
    if (decode_l1pre(d)) {
        d->st.l1_errors++;
        return -1;
    }
    const struct l1pre* e = &d->pre;
    d->tr_guess = (e->papr & 2) != 0;
    /* the layout used for the P2 symbols assumed a guard and pattern: redo
     * it with the signalled ones if needed */
    const int key = (e->bwt_ext << 8) | (e->pilot_pattern << 4) | ((e->papr & 2) ? 2 : 0) | e->guard << 12;
    if (key != d->layout_key) {
        struct layout L;
        if (make_layout(&L, d->n, e->bwt_ext, e->pilot_pattern, (e->papr & 2) != 0, e->guard))
            return -1;
        if (L.ktot != d->L.ktot || L.cp2 != d->L.cp2) {
            if (t2_debug())
                fprintf(stderr, "t2: P2 layout guessed differently, next frame\n");
            /* the P2 cells were laid out differently: next frame */
            free_layout(&d->L);
            d->L = L;
            d->layout_key = key;
            return -1;
        }
        free_layout(&d->L);
        d->L = L;
        d->layout_key = key;
        seed_from_p2(d);
    }
    d->guard_idx = e->guard;
    d->ldata = e->num_data_symbols;
    d->lf = d->L.np2 + d->ldata;
    d->have_pre = 1;
    ensure_cells(d);
    if (decode_l1post(d)) {
        d->st.l1_errors++;
        return -1;
    }
    return 0;
}

/* ---- PLP decoding ----------------------------------------------------------------- */

/* Frame cell index of PLP address a (after the L1 cells). */
static int plp_cell(const drx_dvbt2* d, long a, int l1cells_per_p2)
{
    const struct layout* L = &d->L;
    const int left = L->cp2 - l1cells_per_p2;
    if (a < (long)left * L->np2)
        return (int)(a / left) * L->cp2 + l1cells_per_p2 + (int)(a % left);
    return L->np2 * L->cp2 + (int)(a - (long)left * L->np2);
}

/* Consecutive PLP addresses from a that are consecutive cells (up to the
 * end of a P2 symbol's share, at most max). */
static long plp_run(const drx_dvbt2* d, long a, int l1cells_per_p2, long max)
{
    const struct layout* L = &d->L;
    const int left = L->cp2 - l1cells_per_p2;
    const long r = a < (long)left * L->np2 ? left - a % left : max;
    return r < max ? r : max;
}

/* Demultiplexer of a PLP (clause 6.2.1), NULL for QPSK. */
static const int* plp_mux(const struct plp* q)
{
    const int m = 2 * (q->mod + 1), normal = q->fec_type == 1;
    return m == 4   ? (q->cod == 1 && normal ? MUX16_35 : MUX16)
           : m == 6 ? (q->cod == 1 && normal ? MUX64_35 : MUX64)
           : m == 8 ? (!normal ? MUX256S : q->cod == 1 ? MUX256_35 : q->cod == 2 ? MUX256_23 : MUX256)
                    : NULL;
}

/* Two cells per demultiplexer word (all but QPSK and 256-QAM short). */
static int plp_pair(const struct plp* q)
{
    const int m = 2 * (q->mod + 1);
    return !(m == 8 && q->fec_type == 0) && m > 2;
}

static int setup_plp_fec(struct fecwork* d, const struct plp* q)
{
    const int sf = q->fec_type == 0;
    const int key = (q->cod << 8) | (sf << 4) | q->mod;
    if (key == d->bitmap_key && d->ldpc)
        return 0;
    drx_ldpc_free(d->ldpc);
    drx_bch_free(d->bch);
    d->ldpc = NULL;
    d->bch = NULL;
    if (q->cod > 5 || q->mod > 3 || drx_fec_lookup(sf, PLP_RATES[q->cod], &d->fec))
        return -1;
    /* DVB-T2 has its own 2/3 normal and 3/5 short codes */
    const char* code = !sf && q->cod == 2 ? "2_3_t2" : sf && q->cod == 1 ? "3_5_t2" : PLP_RATES[q->cod];
    d->ldpc = drx_ldpc_new(&d->fec, code);
    d->bch = drx_bch_new(&d->fec);
    if (!d->ldpc || !d->bch)
        return -1;
    /* bit deinterleaver map: interleaver output position -> codeword bit
     * (parity interleaving, then column twist, clause 6.1.3) */
    const int N = d->fec.nldpc, K = d->fec.kldpc, Q = (N - K) / 360;
    const int m = 2 * (q->mod + 1);
    free(d->bitmap);
    d->bitmap = malloc(sizeof(int) * N);
    int* u = malloc(sizeof(int) * N);
    if (!d->bitmap || !u) {
        free(u);
        return -1;
    }
    for (int i = 0; i < K; i++)
        u[i] = i;
    for (int t = 0; t < Q; t++)
        for (int s = 0; s < 360; s++)
            u[K + 360 * t + s] = K + Q * s + t;
    if (m == 2) {
        /* QPSK: no bit interleaving at all (T2-base) */
        for (int i = 0; i < N; i++)
            d->bitmap[i] = i;
    } else {
        const int nc = (m == 8 && sf) ? 8 : 2 * m, nr = N / nc;
        const int* tw = m == 4 ? (sf ? TWIST16S : TWIST16N) : m == 6 ? (sf ? TWIST64S : TWIST64N)
                                                             : (sf ? TWIST256S : TWIST256N);
        int idx = 0;
        for (int c = 0; c < nc; c++)
            for (int r = 0; r < nr; r++) {
                const int row = (tw[c] + r) % nr;
                d->bitmap[row * nc + c] = u[idx++];
            }
    }
    free(u);
    /* the soft value of each codeword bit: interleaver output t is bit e2 of
     * the demultiplexer word t / wbits, i.e. bit mux[e2] of its cells */
    const int ncells = N / m;
    const int* mux = plp_mux(q);
    const int wbits = plp_pair(q) ? 2 * m : m;
    d->stride = (ncells + 31) & ~31;
    if (!d->ci) {
        /* 64800 bits: at most 32400 cells (32416 rounded up, + 9 more for
         * the vector reads); planes of 64800 / m rounded up to 32 bytes */
        d->ci = malloc(sizeof(float) * (32400 + 64));
        d->cq = malloc(sizeof(float) * (32400 + 64));
        d->cc = malloc(sizeof(float) * (32400 + 64));
        d->ck = malloc(sizeof(float) * (32400 + 64));
        d->planes = malloc(65536);
        d->llrsrc = malloc(sizeof(uint16_t) * 64800);
        d->il = malloc(64800);
        if (!d->ci || !d->cq || !d->cc || !d->ck || !d->planes || !d->llrsrc || !d->il) {
            free(d->ci);
            free(d->cq);
            free(d->cc);
            free(d->ck);
            free(d->planes);
            free(d->llrsrc);
            free(d->il);
            d->ci = d->cq = d->cc = d->ck = NULL;
            d->planes = d->il = NULL;
            d->llrsrc = NULL;
            return -1;
        }
    }
    d->ncol = 0;
    d->split = 0;
    if (m == 2) {
        for (int t = 0; t < N; t++)
            d->llrsrc[d->bitmap[t]] = (uint16_t)(t % 2 * d->stride + t / 2);
    } else {
        /* the words are the interleaver rows (wbits = number of columns):
         * element (row, c) is bit mux[c] of the word, i.e. bit mux[c] % m of
         * its cell mux[c] / m */
        const int nc = (m == 8 && sf) ? 8 : 2 * m;
        const int* tw = m == 4 ? (sf ? TWIST16S : TWIST16N) : m == 6 ? (sf ? TWIST64S : TWIST64N)
                                                             : (sf ? TWIST256S : TWIST256N);
        d->ncol = nc;
        d->nrow = N / nc;
        d->split = wbits == 2 * m;
        for (int c = 0; c < nc; c++) {
            d->colsrc[c] = mux[c] % m * d->stride + mux[c] / m * (d->stride / 2);
            d->coltw[c] = tw[c];
        }
    }
    /* cell interleaver basic permutation L0 */
    int nd = 0;
    while ((1 << nd) < ncells)
        nd++;
    free(d->cellperm);
    d->cellperm = malloc(sizeof(int) * ncells);
    if (!d->cellperm)
        return -1;
    static const int TAPS[16][6] = { [11] = { 0, 3 }, [12] = { 0, 2 }, [13] = { 0, 1, 4, 6 },
                                     [14] = { 0, 1, 4, 5, 9, 11 }, [15] = { 0, 1, 2, 12 } };
    static const int NT[16] = { [11] = 2, [12] = 2, [13] = 4, [14] = 6, [15] = 4 };
    int s = 0, qn = 0;
    for (int i = 0; i < (1 << nd) && qn < ncells; i++) {
        if (i < 2) {
            s = 0;
        } else if (i == 2) {
            s = 1;
        } else {
            int b = 0;
            for (int t = 0; t < NT[nd]; t++)
                b ^= (s >> TAPS[nd][t]) & 1;
            s = (s >> 1) | (b << (nd - 2));
        }
        const int v = s | ((i & 1) << (nd - 1));
        if (v < ncells)
            d->cellperm[qn++] = v;
    }
    d->cellperm_n = ncells;
    d->bitmap_key = key;
    return qn == ncells ? 0 : -1;
}

/* Demodulation thread: the PLP cells of the frame, time-deinterleaved, to
 * a free frame buffer of the FEC thread (waits for one). */
static void decode_plp(drx_dvbt2* d)
{
    const struct plp* q = &d->plps[d->sel];
    if (q->type > 2 || q->num_blocks <= 0 || q->num_blocks > MAX_FEC || q->cod > 5 || q->mod > 3)
        return;
    const int m = 2 * (q->mod + 1), N = q->fec_type == 1 ? 64800 : 16200, ncells = N / m;
    const int nblocks = q->num_blocks;
    const long total = (long)ncells * nblocks;
    const struct layout* L = &d->L;
    const int npost_cells = d->pre.post_size;
    const int l1per = (1840 + npost_cells) / L->np2;
    const long avail = (long)(L->cp2 - l1per) * L->np2 + (long)d->ldata * L->cdata + (L->fc ? L->cfc - L->cdata : 0);
    if (q->start + total > avail)
        return;
    struct fecjob* job = NULL;
    pthread_mutex_lock(&d->fec_lock);
    for (;;) {
        for (int k = 0; k < NJOB && !job; k++)
            if (d->job[k].state == JOB_FREE)
                job = &d->job[k];
        if (job)
            break;
        pthread_cond_wait(&d->fec_cv, &d->fec_lock);
    }
    pthread_mutex_unlock(&d->fec_lock);
    if (job->cap < total) {
        free(job->buf);
        free(job->csi);
        job->buf = malloc(sizeof(cf) * total);
        job->csi = malloc(sizeof(float) * total);
        job->cap = job->buf && job->csi ? total : 0;
        if (!job->cap)
            return;
    }
    job->q = *q;
    job->total = total;
    cf* const fecbuf = job->buf;
    float* const fecsci = job->csi;
    /* time deinterleaving: TI-blocks of NFEC_TI FEC blocks, the smaller
     * first; each written column-wise in 5 columns per FEC block, read
     * row-wise (clause 6.5). Without time interleaving the cells come in
     * order. */
    const int nti = q->time_il_length && q->time_il_type == 0 ? q->time_il_length : 0;
    long a = q->start;
    if (!nti) {
        for (long i = 0; i < total;) {
            const int qi = plp_cell(d, a, l1per);
            const long n = plp_run(d, a, l1per, total - i);
            memcpy(fecbuf + i, d->cells + qi, sizeof(cf) * n);
            memcpy(fecsci + i, d->csi + qi, sizeof(float) * n);
            i += n;
            a += n;
        }
    } else {
        /* 8 rows at a time: when each is a run of consecutive cells, the
         * reads are 8 sequential streams and the 8 cells of a column one
         * write (instead of one cache line per cell) */
        const int nr = ncells / 5;
        long o = 0;
        for (int s = 0; s < nti; s++) {
            const int nfec = nblocks / nti + (s >= nti - nblocks % nti);
            const int nc = 5 * nfec;
            for (int r0 = 0; r0 < nr; r0 += 8) {
                const int R = nr - r0 < 8 ? nr - r0 : 8;
                int qi[8], runs = 1;
                for (int rr = 0; rr < R; rr++) {
                    qi[rr] = plp_cell(d, a + (long)rr * nc, l1per);
                    runs &= plp_run(d, a + (long)rr * nc, l1per, nc) == nc;
                }
                if (runs) {
                    for (int c = 0; c < nc; c++) {
                        cf* oc = fecbuf + o + (long)c * nr + r0;
                        float* os = fecsci + o + (long)c * nr + r0;
                        for (int rr = 0; rr < R; rr++) {
                            oc[rr] = d->cells[qi[rr] + c];
                            os[rr] = d->csi[qi[rr] + c];
                        }
                    }
                } else {
                    for (int rr = 0; rr < R; rr++)
                        for (int c = 0; c < nc; c++) {
                            const int q = plp_cell(d, a + (long)rr * nc + c, l1per);
                            fecbuf[o + (long)c * nr + r0 + rr] = d->cells[q];
                            fecsci[o + (long)c * nr + r0 + rr] = d->csi[q];
                        }
                }
                a += (long)R * nc;
            }
            o += (long)nc * nr;
        }
    }
    pthread_mutex_lock(&d->fec_lock);
    job->seq = d->next_seq++;
    job->cb = d->cb;
    job->ctx = d->ctx;
    job->state = JOB_READY;
    pthread_cond_broadcast(&d->fec_cv);
    pthread_mutex_unlock(&d->fec_lock);
}

/* FEC thread: one frame's PLP, from the time-deinterleaved cells to TS. */
static void decode_job(struct fecwork* d, struct fecjob* job, int max_iter)
{
    const struct plp* q = &job->q;
    job->nblocks = 0;
    if (setup_plp_fec(d, q))
        return;
    const int m = 2 * (q->mod + 1), N = d->fec.nldpc, ncells = N / m;
    const int nblocks = q->num_blocks;
    const int kb = d->fec.kbch / 8;
    if (job->bbcap < nblocks * kb) {
        free(job->bb);
        free(job->ok);
        free(job->err);
        free(job->sig);
        job->bb = malloc((size_t)nblocks * kb);
        job->ok = malloc(nblocks);
        job->err = malloc(sizeof(float) * nblocks);
        job->sig = malloc(sizeof(float) * nblocks);
        job->bbcap = job->bb && job->ok && job->err && job->sig ? nblocks * kb : 0;
        if (!job->bbcap)
            return;
    }
    job->kb = kb;
    const int nti = q->time_il_length && q->time_il_type == 0 ? q->time_il_length : 0;

    /* per FEC block: cell deinterleaving, demapping, bit deinterleaving */
    const int nb = m / 2;
    const double th = q->rotation ? ROT_DEG[q->mod] * M_PI / 180 : 0;
    const float ct = (float)cos(th), sn = (float)sin(th);
    const float norm = m == 2 ? (float)M_SQRT2 : m == 4 ? sqrtf(10) : m == 6 ? sqrtf(42) : sqrtf(170);
    const int rot = q->rotation != 0;
    float *const ci = d->ci, *const cq = d->cq, *const cc = d->cc, *const ck = d->ck;
    int8_t* const planes = d->planes;
    const int stride = d->stride, split = d->split, half = stride / 2;
    const uint16_t* const llrsrc = d->llrsrc;
    const v8f vct = vsplat(ct), vsn = vsplat(sn), vnorm = vsplat(norm), eps = vsplat(1e-12f);
    const v8f ct2 = vsplat(ct * ct), sn2 = vsplat(sn * sn);
    const v8f amax = vsplat((float)((1 << (nb - 1)) - 1)), s2 = vsplat(norm * norm * 0.5f);
    const v8i iota = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int lanes = drx_ldpc_lanes(), nl = 0;
    long blockstart = 0;
    int nd = 0;
    while ((1 << nd) < ncells)
        nd++;
    /* cell deinterleaver shift P(r): bit-reversed counter, restarted with
     * each TI-block (clause 6.4) */
    int ti_s = 0, ti_left = 0, kcount = 0;
    for (int b = 0; b < nblocks; b++) {
        int P = 0;
        if (nti) {
            while (ti_left == 0 && ti_s < nti) {
                ti_left = nblocks / nti + (ti_s >= nti - nblocks % nti);
                ti_s++;
                kcount = 0;
            }
            for (;;) {
                int v = 0;
                for (int j = 0; j < nd; j++)
                    v |= ((kcount >> j) & 1) << (nd - 1 - j);
                kcount++;
                if (v < ncells) {
                    P = v;
                    break;
                }
            }
            ti_left--;
        }
        const cf* in = job->buf + blockstart;
        const float* ic = job->csi + blockstart;
        for (int i = 0; i < ncells; i++) {
            int src = d->cellperm[i] + P;
            src -= src >= ncells ? ncells : 0;
            ci[i] = crealf(in[src]);
            cq[i] = cimagf(in[src]);
            cc[i] = ic[src];
        }
        /* the Q of the last point is in the first cell; whole vectors of 32
         * (cells up to stride + 8 are read) */
        for (int i = 0; i < stride - ncells + 9; i++) {
            ci[ncells + i] = ci[i];
            cq[ncells + i] = cq[i];
            cc[ncells + i] = cc[i];
        }
        blockstart += ncells;
        /* demap, 8 points at a time: point i has its I in cell i and its Q
         * in cell i + 1 (rotated) or i; derotated point w = e^(-j th)(u + j v)
         * and per-axis noise weights kr, ki in place of I, Q, channel state
         * (cells i + 1 .. i + 8 are read before i .. i + 7 are written) */
        const int o = rot;
        v8f verr = vsplat(0), vsig = vsplat(0);
        for (int i = 0; i < stride; i += 8) {
            const v8f u = ldf(ci + i), v = ldf(cq + i + o);
            const v8f c1 = ldf(cc + i) + eps, c2 = ldf(cc + i + o) + eps;
            const v8f wr = (vct * u + vsn * v) * vnorm, wi = (vct * v - vsn * u) * vnorm;
            v8f kr = c1, ki = c1;
            if (rot) {
                /* 1 / (ct^2 / c1 + sn^2 / c2) and 1 / (sn^2 / c1 + ct^2 / c2) */
                const v8f c12 = c1 * c2;
                kr = c12 / (ct2 * c2 + sn2 * c1);
                ki = c12 / (sn2 * c2 + ct2 * c1);
            }
            /* MER on the derotated point: nearest odd level 2 floor(|w| / 2) + 1,
             * at most 2 na - 1 */
            const v8f awr = vabs(wr), awi = vabs(wi);
            const v8f ar = 2 * vtrunc(vmin(awr * 0.5f, amax)) + 1;
            const v8f ai = 2 * vtrunc(vmin(awi * 0.5f, amax)) + 1;
            const v8f er = awr - ar, ei = awi - ai;
            const v8i in_block = iota + i < ncells;
            verr += vand(er * er * kr + ei * ei * ki, in_block);
            vsig += vand(s2 * (kr + ki), in_block);
            stf(ci + i, wr);
            stf(cq + i, wi);
            stf(cc + i, kr);
            stf(ck + i, ki);
        }
        float err = 0, sig = 0;
        for (int l = 0; l < 8; l++) {
            err += verr[l];
            sig += vsig[l];
        }
        job->err[b] = err;
        job->sig[b] = sig;
        const float n0u = err / ncells + 1e-6f;
        const v8f k = vsplat(8.0f / n0u);
        /* max-log soft values (d1 - d0) / 4 of each PAM axis (2 nb levels per
         * sign, piecewise linear in the level units): w, |w| - t, ||w| - t| -
         * t / 2 ... (t = 2^(nb - 1)), times the axis weight, to 8 bits: plane
         * 2 j holds bit j of I, plane 2 j + 1 bit j of Q (with two cells per
         * demultiplexer word: the even cells, then from stride / 2 the odd
         * ones, so that each interleaver column is a run of one plane) */
        for (int i = 0; i < stride; i += 32) {
            v8f ur[4], ui[4], kr[4], ki[4], vr[4], vi[4];
            for (int l = 0; l < 4; l++) {
                ur[l] = ldf(ci + i + 8 * l);
                ui[l] = ldf(cq + i + 8 * l);
                kr[l] = ldf(cc + i + 8 * l);
                ki[l] = ldf(ck + i + 8 * l);
            }
            for (int j = 0; j < nb; j++) {
                const v8f t = vsplat((float)(1 << (nb - j)));
                for (int l = 0; l < 4; l++) {
                    if (j) {
                        ur[l] = vabs(ur[l]) - t;
                        ui[l] = vabs(ui[l]) - t;
                    }
                    vr[l] = (ur[l] * kr[l]) * k;
                    vi[l] = (ui[l] * ki[l]) * k;
                }
                if (split) {
                    to_llr32_split(planes + 2 * j * stride + i / 2, planes + 2 * j * stride + half + i / 2, vr);
                    to_llr32_split(planes + (2 * j + 1) * stride + i / 2,
                                   planes + (2 * j + 1) * stride + half + i / 2, vi);
                } else {
                    to_llr32(planes + 2 * j * stride + i, vr);
                    to_llr32(planes + (2 * j + 1) * stride + i, vi);
                }
            }
        }
        /* demultiplexing and bit deinterleaving into the codeword */
        int8_t* llr = d->llr[nl];
        if (d->ncol) {
            /* columns: element r of column c is in row (tw[c] + r) mod nrow;
             * the column-wise order is the codeword's for the K information
             * bits, and parity bit 360 t + s is bit Q s + t (clause 6.1.2) */
            const int nr = d->nrow, K = d->fec.kldpc, Q = (N - K) / 360;
            int8_t* il = d->il;
            for (int c = 0; c < d->ncol; c++) {
                const int8_t* sp = planes + d->colsrc[c];
                const int t0 = d->coltw[c];
                memcpy(il + c * nr, sp + t0, nr - t0);
                memcpy(il + c * nr + nr - t0, sp, t0);
            }
            memcpy(llr, il, K);
            for (int t = 0; t < Q; t++) {
                const int8_t* src = il + K + 360 * t;
                int8_t* dst = llr + K + t;
                for (int s = 0; s < 360; s++)
                    dst[Q * s] = src[s];
            }
        } else {
            for (int c = 0; c < N; c++)
                llr[c] = planes[llrsrc[c]];
        }
        nl++;
        if (nl == lanes || b == nblocks - 1) {
            const int it = drx_ldpc_decode(d->ldpc, (const int8_t* const*)d->llr, d->cw, nl, max_iter);
            (void)it;
            for (int f = 0; f < nl; f++) {
                const int fb = b - nl + 1 + f;
                job->ok[fb] = drx_bch_decode(d->bch, d->cw[f]) >= 0;
                memcpy(job->bb + (size_t)fb * kb, d->cw[f], kb);
            }
            nl = 0;
        }
    }
    job->nblocks = nblocks;
}

/* In frame order: the decoded BBFRAMEs of a frame to TS, and statistics. */
static void emit_job(drx_dvbt2* d, const struct fecjob* job)
{
    for (int b = 0; b < job->nblocks; b++) {
        d->mer_err += 0.1f * (job->err[b] - d->mer_err);
        d->mer_sig += 0.1f * (job->sig[b] - d->mer_sig);
        d->st.fec_blocks++;
        if (!job->ok[b]) {
            d->st.fec_failed++;
            continue;
        }
        const int kb = job->kb;
        memcpy(d->bbframe, job->bb + (size_t)b * kb, kb);
        drx_bb_descramble(d->bbframe, kb);
        drx_bbheader h;
        const int nts = drx_bbdec_frame(d->bb, d->bbframe, kb, d->ts, &h);
        for (int t = 0; t + 188 <= nts; t += 188) {
            d->st.packets++;
            if (d->ts[t + 1] & 0x80)
                d->st.crc_errors++;
            if (job->cb)
                job->cb(job->ctx, d->ts + t, 188);
        }
    }
    d->st.locked = d->st.packets > 0;
}

static void* fec_thread(void* arg)
{
    struct fecwork* w = arg;
    drx_dvbt2* d = w->d;
    pthread_mutex_lock(&d->fec_lock);
    for (;;) {
        /* the oldest frame waiting */
        struct fecjob* job = NULL;
        for (int k = 0; k < NJOB; k++)
            if (d->job[k].state == JOB_READY && (!job || d->job[k].seq < job->seq))
                job = &d->job[k];
        if (!job) {
            if (d->fec_stop)
                break;
            pthread_cond_wait(&d->fec_cv, &d->fec_lock);
            continue;
        }
        job->state = JOB_BUSY;
        /* another frame already waiting: the FEC threads are behind */
        int waiting = 0;
        for (int k = 0; k < NJOB; k++)
            waiting += d->job[k].state == JOB_READY;
        pthread_mutex_unlock(&d->fec_lock);
        decode_job(w, job, waiting ? LDPC_ITER_BUSY : LDPC_ITER);
        pthread_mutex_lock(&d->fec_lock);
        while (job->seq != d->emit_seq)
            pthread_cond_wait(&d->fec_cv, &d->fec_lock);
        pthread_mutex_unlock(&d->fec_lock);
        emit_job(d, job);
        pthread_mutex_lock(&d->fec_lock);
        d->emit_seq++;
        job->state = JOB_FREE;
        pthread_cond_broadcast(&d->fec_cv);
    }
    pthread_mutex_unlock(&d->fec_lock);
    return NULL;
}

void drx_dvbt2_flush(drx_dvbt2* d)
{
    if (!d->fec_ok)
        return;
    pthread_mutex_lock(&d->fec_lock);
    while (d->emit_seq != d->next_seq)
        pthread_cond_wait(&d->fec_cv, &d->fec_lock);
    pthread_mutex_unlock(&d->fec_lock);
}

/* ---- driver ------------------------------------------------------------------- */

/* Drop the samples before pos. */
/* Drops the samples before pos, but a margin: the end-of-frame check looks
 * for the next P1 a little before where it is expected. */
#define KEEP_BEFORE 1024
static void compact(drx_dvbt2* d)
{
    int drop = (d->pos < d->nbuf ? d->pos : d->nbuf) - KEEP_BEFORE;
    if (drop <= 0)
        return;
    memmove(d->buf, d->buf + drop, sizeof(cf) * (d->nbuf - drop));
    d->nbuf -= drop;
    d->pos -= drop;
}

static int reserve(drx_dvbt2* d, int n)
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

/* Acquisition: P1, then the frame starts. */
static int acquire(drx_dvbt2* d)
{
    for (;;) {
        const int s = find_p1(d, P1_SCAN);
        if (s == -2)
            return 0;
        if (s < 0)
            continue;
        if (s + P1_LEN + 4 * (32768 + 8192) + 32768 > d->nbuf) {
            d->pos = s; /* wait for the symbols after it */
            return 0;
        }
        if (decode_p1(d, s) || detect_guard(d, s) < 0) {
            if (t2_debug() && d->s1 == 0)
                fprintf(stderr, "t2: guard not found after the P1 (FFT %d)\n", d->n);
            d->pos = s + 1024;
            continue;
        }
        if (d->fft.n != d->n) {
            fft_free(&d->fft);
            if (fft_init(&d->fft, d->n))
                return 0;
        }
        /* carrier mode: energy of the extended carriers in the first P2 */
        d->pos = s + P1_LEN;
        if (t2_debug())
            fprintf(stderr, "t2: guard %d/%d, f %.7f, P2 at %d\n", GI_NUM[d->guard_idx], GI_DEN[d->guard_idx], d->f, d->pos);
        d->phase = -2 * M_PI * d->f * d->pos;
        int ext = 0;
        if (d->n >= 8192) {
            const int fi = fft_idx(d->n);
            d->g = d->n * GI_NUM[d->guard_idx] / GI_DEN[d->guard_idx];
            demod_symbol(d, KTOT_E[fi]);
            const int kx = (KTOT_E[fi] - KTOT_N[fi]) / 2;
            float eo = 0, ei = 0;
            for (int k = 0; k < kx; k++)
                eo += crealf(d->x[k] * conjf(d->x[k])) + crealf(d->x[KTOT_E[fi] - 1 - k] * conjf(d->x[KTOT_E[fi] - 1 - k]));
            for (int k = kx; k < 3 * kx; k++)
                ei += crealf(d->x[k] * conjf(d->x[k])) + crealf(d->x[KTOT_E[fi] - 1 - k] * conjf(d->x[KTOT_E[fi] - 1 - k]));
            ext = eo > 0.3f * ei;
        }
        /* provisional layout for the P2 symbols (pattern and guard only
         * matter for the data symbols) */
        free_layout(&d->L);
        int pp = 1;
        while (pp <= 8 && make_layout(&d->L, d->n, ext, pp, d->tr_guess, d->guard_idx))
            pp++;
        if (pp > 8)
            return 0;
        d->layout_key = -1;
        d->ldata = 0;
        d->l = 0;
        d->tracked = 0;
        d->drift = 0;
        d->tau = 0;
        d->have_cpp = 0;
        d->state = ST_FRAME;
        d->frame_ok = 0;
        return 1;
    }
}

void drx_dvbt2_gap(drx_dvbt2* d)
{
    restart(d);
    d->pos = d->nbuf;
}

static int set_step(drx_dvbt2* d, double step)
{
    if (step != d->cur_step && d->cur_step > 0 && d->state == ST_FRAME) {
        const double truth = d->cur_step * (1 + d->drift / d->sym_len);
        d->drift = (truth / step - 1) * d->sym_len;
    }
    d->cur_step = step;
    return 0;
}

static void run(drx_dvbt2* d)
{
    for (;;) {
        if (d->state == ST_SEARCH) {
            if (!acquire(d))
                break;
            continue;
        }
        const struct layout* L = &d->L;
        /* end of frame: the next P1 must be there */
        if (d->frame_ok && d->l == d->lf) {
            if (d->frame_ok == 1) {
                /* frames demodulated before the sample clock was known (and
                 * corrected) after an acquisition are not decoded */
                if (d->sel >= 0 && d->tracked_start >= CLOCK_SETTLE)
                    decode_plp(d);
                d->st.frames++;
                d->frame_ok = 2;
            }
            if (d->pos + P1_LEN + 4096 + 64 > d->nbuf)
                break;
            /* check the P1 near the expected place, re-time on it (its
             * fractional offset estimate is not used: the loop tracks) */
            const int save = d->pos;
            const double f = d->f;
            d->pos -= 32;
            /* a P1 at most 64 samples on: scanning to 1024 past it (where
             * the search stops) gives the same as a full scan */
            const int s = find_p1(d, 4096);
            d->f = f;
            if (s < 0 || s > save + 32) {
                d->pos = save;
                restart(d);
                continue;
            }
            d->pos = s + P1_LEN;
            d->tau = 0;
            d->phase = fmod(-2 * M_PI * d->f * d->pos, 2 * M_PI);
            d->l = 0;
            d->frame_ok = 0;
            continue;
        }
        if (d->pos + d->sym_len + 8 > d->nbuf)
            break;
        demod_symbol(d, L->ktot);
        if (d->l < L->np2) {
            ensure_cells(d);
            p2_symbol(d);
            if (d->l == L->np2 - 1) {
                if (frame_l1(d)) {
                    /* without L1, skip to the next frame start by searching */
                    restart(d);
                    d->pos += d->sym_len;
                    continue;
                }
                d->frame_ok = 1;
                d->tracked_start = d->tracked;
            }
        } else {
            const int closing = d->L.fc && d->l == d->lf - 1;
            data_symbol(d, closing);
        }
        d->l++;
        /* window position follows the transmitter's clock */
        int adj = 0;
        if (d->tau > 1.5 || d->tau < -1.5)
            adj = (int)lrint(d->tau);
        if (adj) {
            d->tau -= adj;
            shift_refs(d, -adj);
        }
        advance(d, d->sym_len + adj);
    }
    if (d->pos > d->cap / 2)
        compact(d);
}

int drx_dvbt2_resample(drx_dvbt2* d, const float* iq, int n, float* out, int max_out, double* step)
{
    *step = d->step;
    resamp_set_ratio(&d->rs, *step);
    return resamp_run(&d->rs, (const cf*)iq, n, (cf*)out, max_out);
}

void drx_dvbt2_demod(drx_dvbt2* d, const float* x, int n, double step, drx_dvbt2_cb cb, void* ctx)
{
    d->cb = cb;
    d->ctx = ctx;
    set_step(d, step);
    if (reserve(d, n))
        return;
    memcpy(d->buf + d->nbuf, x, sizeof(cf) * n);
    if (d->inv)
        for (int i = 0; i < n; i++)
            d->buf[d->nbuf + i] = conjf(d->buf[d->nbuf + i]);
    d->nbuf += n;
    run(d);
}

void drx_dvbt2_process(drx_dvbt2* d, const float* iq, int n, drx_dvbt2_cb cb, void* ctx)
{
    d->cb = cb;
    d->ctx = ctx;
    const int room = (int)(n / (d->step_nominal * 0.99)) + 64;
    if (reserve(d, room))
        return;
    double step;
    const int got = drx_dvbt2_resample(d, iq, n, (float*)(d->buf + d->nbuf), d->cap - d->nbuf, &step);
    if (d->inv)
        for (int i = 0; i < got; i++)
            d->buf[d->nbuf + i] = conjf(d->buf[d->nbuf + i]);
    d->nbuf += got;
    set_step(d, step);
    run(d);
}
