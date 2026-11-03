/*
 * DVB-S2 / S2X forward error correction parameters and BCH decoder.
 */
#ifndef DVBRX_FEC_H
#define DVBRX_FEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int short_frame; /* 0: normal (64800 bits), 1: short (16200 bits) */
    int nldpc;       /* LDPC codeword length */
    int kldpc;       /* = nbch */
    int kbch;        /* BBFRAME length */
    int t;           /* BCH error-correcting capability */
} drx_fec_params;

/*
 * Look up the FEC parameters of a code rate given as "3_4", "9_10",
 * "2_9_VLSNR", "13_45", ... (gr-dvbs2rx naming). Returns 0 on success.
 */
int drx_fec_lookup(int short_frame, const char* rate, drx_fec_params* p);

typedef struct drx_bch drx_bch;

drx_bch* drx_bch_new(const drx_fec_params* p);
void drx_bch_free(drx_bch* b);

/*
 * Decode a BCH codeword in place: kldpc bits, packed MSB first (the first
 * kbch bits are the BBFRAME). Returns the number of corrected bits, or -1
 * if the codeword is uncorrectable (left untouched).
 */
int drx_bch_decode(drx_bch* b, uint8_t* cw);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_FEC_H */
