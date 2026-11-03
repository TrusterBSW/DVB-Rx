/*
 * DVB-T (EN 300 744) receiver: complex baseband samples in, MPEG-TS out.
 *
 * Resampling to the elementary rate (8/7 of the channel bandwidth), mode
 * (2K / 8K) and guard interval detection on the cyclic prefix, fractional
 * and integer carrier offset, scattered-pilot channel estimation, common
 * phase error and sample clock tracking on the continual pilots, TPS
 * decoding (constellation, code rate, hierarchy), symbol and bit
 * deinterleaving, soft demapping weighted by the channel state, Viterbi,
 * outer deinterleaver, RS(204,188) and energy dispersal removal. With
 * hierarchical modulation, the high-priority stream is decoded.
 */
#ifndef DVBRX_DVBT_H
#define DVBRX_DVBT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int locked;      /* TS sync acquired */
    int fft;         /* 2048 or 8192, 0 while searching */
    int guard;       /* guard interval denominator: 4, 8, 16, 32 */
    int qam;         /* 4, 16 or 64, 0 until the TPS is decoded */
    int rate;        /* code rate as enum drx_dvbs_rate (1 = 1/2 .. 5 = 7/8) */
    int hierarchy;   /* 0: none, else alpha (1, 2, 4) */
    float snr_db;    /* MER on the data cells */
    float freq;      /* carrier offset, cycles per input sample */
    float clock_ppm; /* sample clock offset of the transmitter */
    uint64_t symbols;      /* OFDM symbols demodulated */
    uint64_t packets;      /* TS packets output */
    uint64_t rs_corrected; /* bytes fixed by Reed-Solomon */
    uint64_t rs_failed;    /* packets output with the error indicator set */
    uint64_t resyncs;      /* acquisitions restarted */
} drx_dvbt_stats;

typedef struct drx_dvbt drx_dvbt;

/* Called with whole 188-byte TS packets. */
typedef void (*drx_dvbt_cb)(void* ctx, const uint8_t* ts, int len);

/* sample_rate: of the samples fed; bandwidth: channel bandwidth in Hz
 * (8, 7, 6 or 5 MHz). threaded: Viterbi and outer layer on their own
 * thread (the TS callback is then called from it). */
drx_dvbt* drx_dvbt_new(double sample_rate, double bandwidth, int threaded);
void drx_dvbt_free(drx_dvbt* d);

/* Feed n complex samples (interleaved I/Q). */
void drx_dvbt_process(drx_dvbt* d, const float* iq, int n, drx_dvbt_cb cb, void* ctx);

/* The same in two stages, for two threads: resampling (out gets at most
 * max_out samples at the elementary rate; n / ratio + 2 is enough; *step
 * receives the resampling step used, which follows the transmitter's
 * clock), then demodulation of those, given that step. */
int drx_dvbt_resample(drx_dvbt* d, const float* iq, int n, float* out, int max_out,
                      double* step);
void drx_dvbt_demod(drx_dvbt* d, const float* x, int n, double step, drx_dvbt_cb cb,
                    void* ctx);

/* Samples were lost before the next ones: acquire again. */
void drx_dvbt_gap(drx_dvbt* d);

/* Decode what is buffered (end of input). */
void drx_dvbt_flush(drx_dvbt* d);

void drx_dvbt_get_stats(const drx_dvbt* d, drx_dvbt_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_DVBT_H */
