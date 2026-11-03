/*
 * DVB-S2 soft demapper and bit deinterleaver: XFECFRAME symbols in, 8-bit
 * LLRs of the LDPC codeword out.
 */
#ifndef DVBRX_DEMAP_H
#define DVBRX_DEMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A MODCOD as signalled in the PLHEADER. */
typedef struct {
    int pls;                   /* PLS code with the pilot bit cleared */
    const char* constellation; /* "QPSK", "8PSK", "16APSK", "64APSK", ... */
    const char* rate;          /* "3_4", "13_45", "132_180", ... */
    int short_frame;
    int bits; /* bits per symbol */
} drx_modcod;

/* Look up a PLS code (DVB-S2 or DVB-S2X, pilot bit ignored). Returns 0 if
 * the MODCOD is supported. */
int drx_pls_lookup(int pls, drx_modcod* m);

typedef struct drx_demap drx_demap;

/* constellation and rate as in drx_modcod; rate as in
 * drx_fec_lookup(). Returns NULL for an unknown MODCOD. */
drx_demap* drx_demap_new(const char* constellation, const char* rate, int short_frame);
void drx_demap_free(drx_demap* d);

int drx_demap_bits(const drx_demap* d);    /* bits per symbol */
int drx_demap_symbols(const drx_demap* d); /* symbols per XFECFRAME */

/*
 * Demap one XFECFRAME: drx_demap_symbols() interleaved I/Q symbols at unit
 * average energy, noise power n0 per complex symbol. Writes the nldpc LLRs
 * in codeword order, positive for a 0 bit, scaled by 2 and saturated to
 * +-127 (the LDPC decoder's input scale).
 */
void drx_demap_frame(const drx_demap* d, const float* sym, float n0, int8_t* llr);

/* Constellation points (unit average energy) indexed by bit label.
 * re and im must hold 256 entries. Returns the number of points. */
int drx_demap_points(const drx_demap* d, float* re, float* im);

/* Index of the constellation point closest to (x, y). */
int drx_demap_nearest(const drx_demap* d, float x, float y);

/* Decision-directed noise power estimate (biased low at low SNR). */
float drx_demap_estimate_n0(const drx_demap* d, const float* sym, int nsym);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_DEMAP_H */
