/*
 * DVB-T2 (EN 302 755) receiver: complex baseband samples in, MPEG-TS out.
 *
 * T2-base profile, SISO, with any FFT size (1K .. 32K), guard interval,
 * pilot pattern and carrier mode; L1-pre and L1-post decoding; one data PLP
 * (Type 1) decoded, with time and cell deinterleaving, rotated
 * constellations, 16K / 64K LDPC and BCH; TS reconstruction from the
 * baseband frames.
 */
#ifndef DVBRX_DVBT2_H
#define DVBRX_DVBT2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int locked;      /* TS packets coming out */
    int fft;         /* 1024 .. 32768, 0 while searching */
    int guard_num, guard_den; /* guard interval fraction */
    int extended;    /* extended carrier mode */
    int pilot_pattern; /* 1 .. 8 */
    int num_plp;
    int plp_id;      /* PLP decoded, -1 until known */
    int qam;         /* of that PLP: 4, 16, 64, 256 */
    const char* rate;  /* its code rate, "2_3"... */
    int short_frame;   /* 16K LDPC */
    int rotated;
    float snr_db;    /* MER of the data cells */
    float freq;      /* carrier offset, cycles per input sample */
    float clock_ppm; /* sample clock offset of the transmitter */
    uint64_t frames;       /* T2-frames demodulated */
    uint64_t l1_errors;    /* frames whose L1 failed to decode */
    uint64_t fec_blocks;   /* FEC blocks decoded */
    uint64_t fec_failed;   /* FEC blocks with LDPC or BCH failure */
    uint64_t packets;      /* TS packets output */
    uint64_t crc_errors;   /* TS packets with the error indicator set */
    uint64_t resyncs;      /* acquisitions restarted */
} drx_dvbt2_stats;

typedef struct drx_dvbt2 drx_dvbt2;

/* Called with whole 188-byte TS packets. */
typedef void (*drx_dvbt2_cb)(void* ctx, const uint8_t* ts, int len);

/* sample_rate: of the samples fed; bandwidth: channel bandwidth in Hz
 * (1.7, 5, 6, 7, 8 or 10 MHz). plp_id: PLP to decode, -1 for the first
 * data PLP. */
drx_dvbt2* drx_dvbt2_new(double sample_rate, double bandwidth, int plp_id);
void drx_dvbt2_free(drx_dvbt2* d);

/* Feed n complex samples (interleaved I/Q). */
void drx_dvbt2_process(drx_dvbt2* d, const float* iq, int n, drx_dvbt2_cb cb, void* ctx);

/* The same in two stages, for two threads (see drx_dvbt_resample). */
int drx_dvbt2_resample(drx_dvbt2* d, const float* iq, int n, float* out, int max_out,
                       double* step);
void drx_dvbt2_demod(drx_dvbt2* d, const float* x, int n, double step, drx_dvbt2_cb cb,
                     void* ctx);

/* Samples were lost before the next ones: acquire again. */
void drx_dvbt2_gap(drx_dvbt2* d);
/* Waits until the frames demodulated so far are decoded (their TS is out):
 * PLP decoding runs on a thread of its own. */
void drx_dvbt2_flush(drx_dvbt2* d);

void drx_dvbt2_get_stats(const drx_dvbt2* d, drx_dvbt2_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_DVBT2_H */
