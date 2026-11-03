/*
 * Arbitrary-ratio resampler (internal), for OFDM: the SDR rate (61.44/N
 * MHz on some devices) to the elementary rate of the standard (64/7 MHz
 * for 8 MHz channels), the ratio being trimmed on the fly to follow the
 * transmitter's sample clock.
 *
 * Polyphase windowed-sinc low-pass, RESAMP_PHASES phases of RESAMP_TAPS
 * taps, nearest phase.
 */
#ifndef DVBRX_RESAMP_H
#define DVBRX_RESAMP_H

#include <complex.h>

#define RESAMP_TAPS 32
#define RESAMP_PHASES 512

struct resamp {
    double step; /* input samples per output sample */
    double t;    /* position of the next output in the history, input samples */
    float (*h)[2 * RESAMP_TAPS]; /* each tap twice (I, Q), to filter interleaved samples */
    float complex* hist; /* input history */
    int nhist, cap;
};

/* ratio: input rate / output rate (0.5 .. 2); cutoff: one-sided passband
 * edge, in output Nyquist units (e.g. 0.85 for an OFDM signal filling 85%
 * of the output band). */
int resamp_init(struct resamp* r, double ratio, double cutoff);
void resamp_free(struct resamp* r);
/* Changes the ratio for the outputs to come (clock tracking). */
static inline void resamp_set_ratio(struct resamp* r, double ratio) { r->step = ratio; }
/* Consumes all n inputs; returns the number of outputs written (at most
 * max_out; the inputs beyond are kept for the next call). */
int resamp_run(struct resamp* r, const float complex* in, int n, float complex* out, int max_out);

#endif
