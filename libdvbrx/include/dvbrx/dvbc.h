/*
 * DVB-C (EN 300 429) receiver back half: QAM symbols (one per symbol,
 * after the front-end, roll-off 0.15) in, MPEG-TS out.
 *
 * Coarse carrier frequency from the 4th power of the symbols, blind (CMA)
 * then decision-directed equaliser, decision-directed PLL, differential
 * decoding of the quadrant bits (no phase ambiguity left), packet sync,
 * convolutional deinterleaver (I=12, M=17), RS(204,188) and energy
 * dispersal removal.
 */
#ifndef DVBRX_DVBC_H
#define DVBRX_DVBC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constellation size; DRX_QAM_AUTO tries them in turn. */
enum drx_qam {
    DRX_QAM_AUTO = 0,
    DRX_QAM_16 = 16,
    DRX_QAM_32 = 32,
    DRX_QAM_64 = 64,
    DRX_QAM_128 = 128,
    DRX_QAM_256 = 256,
};

typedef struct {
    int locked;      /* TS sync acquired */
    int qam;         /* constellation in use, 0 while searching */
    float snr_db;    /* MER, from the decision errors */
    float freq;      /* carrier frequency, cycles per symbol */
    uint64_t packets;      /* TS packets output */
    uint64_t rs_corrected; /* bytes fixed by Reed-Solomon */
    uint64_t rs_failed;    /* packets output with the error indicator set */
    uint64_t resyncs;      /* acquisitions restarted */
    int inverted;          /* spectrum taken as inverted (conjugated input) */
} drx_dvbc_stats;

typedef struct drx_dvbc drx_dvbc;

/* Called with whole 188-byte TS packets. */
typedef void (*drx_dvbc_cb)(void* ctx, const uint8_t* ts, int len);

drx_dvbc* drx_dvbc_new(enum drx_qam qam);
void drx_dvbc_free(drx_dvbc* d);

/* Spectral inversion, as enum fe_spectral_inversion: 0 normal, 1 inverted,
 * 2 auto (the default: each failed acquisition round, i.e. after the
 * constellations to try, switches it; cable networks carry both). */
void drx_dvbc_set_inversion(drx_dvbc* d, int mode);

/* Feed n symbols (interleaved I/Q, about unit average energy). */
void drx_dvbc_process(drx_dvbc* d, const float* sym, int n, drx_dvbc_cb cb, void* ctx);

/* The symbols fed from now on are shifted by -delta cycles/symbol (carrier
 * correction applied upstream). */
void drx_dvbc_shift(drx_dvbc* d, double delta);

void drx_dvbc_get_stats(const drx_dvbc* d, drx_dvbc_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_DVBC_H */
