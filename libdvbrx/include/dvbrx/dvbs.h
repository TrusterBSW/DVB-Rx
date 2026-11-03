/*
 * DVB-S (EN 300 421) receiver back half: QPSK symbols (one per symbol,
 * after the front-end) in, MPEG-TS out.
 *
 * Carrier recovery (4th-power frequency estimate, decision-directed PLL),
 * automatic search of the code rate, phase ambiguity, I/Q swap and
 * puncturing phase, K=7 Viterbi decoder, byte sync, convolutional
 * deinterleaver (I=12, M=17), RS(204,188) and energy dispersal removal.
 */
#ifndef DVBRXS_H
#define DVBRXS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Code rates, as k/n: 1/2, 2/3, 3/4, 5/6, 7/8. */
enum drx_dvbs_rate {
    DRX_DVBS_AUTO = 0,
    DRX_DVBS_1_2,
    DRX_DVBS_2_3,
    DRX_DVBS_3_4,
    DRX_DVBS_5_6,
    DRX_DVBS_7_8,
};

typedef struct {
    int locked;      /* TS sync acquired */
    int rate;        /* enum drx_dvbs_rate in use, 0 while searching */
    float snr_db;    /* Es/N0 from the decision errors */
    float freq;      /* carrier frequency, cycles per symbol */
    uint64_t packets;     /* TS packets output */
    uint64_t rs_corrected; /* bytes fixed by Reed-Solomon */
    uint64_t rs_failed;    /* packets output with the error indicator set */
    uint64_t resyncs;      /* acquisitions restarted */
} drx_dvbs_stats;

typedef struct drx_dvbs drx_dvbs;

/* Called with whole 188-byte TS packets. */
typedef void (*drx_dvbs_cb)(void* ctx, const uint8_t* ts, int len);

/* threaded: run the Viterbi / RS back end on its own thread (the TS
 * callback is then called from that thread). */
drx_dvbs* drx_dvbs_new(enum drx_dvbs_rate rate, int threaded);
void drx_dvbs_free(drx_dvbs* d);

/* Feed n symbols (interleaved I/Q, unit average energy). */
void drx_dvbs_process(drx_dvbs* d, const float* sym, int n, drx_dvbs_cb cb, void* ctx);

/* Decode what is buffered (end of input). */
void drx_dvbs_flush(drx_dvbs* d);

/* The symbols fed from now on are shifted by -delta cycles/symbol (carrier
 * correction applied upstream). */
void drx_dvbs_shift(drx_dvbs* d, double delta);

void drx_dvbs_get_stats(const drx_dvbs* d, drx_dvbs_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRXS_H */
