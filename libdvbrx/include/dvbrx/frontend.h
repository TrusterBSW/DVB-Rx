/*
 * Receiver front-end: matched filter + symbol timing recovery + AGC.
 *
 * Takes complex baseband samples at an arbitrary (>= ~1.5) number of samples
 * per symbol and outputs one complex sample per symbol, normalised to unit
 * average energy. An optional frequency shift (drx_frontend_set_freq) runs
 * ahead of the matched filter.
 */
#ifndef DVBRX_FRONTEND_H
#define DVBRX_FRONTEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct drx_frontend drx_frontend;

typedef struct {
    float sps;       /* input samples per symbol */
    float rolloff;   /* roll-off of the RRC matched filter */
    int rrc_delay;   /* matched filter half-length, in symbols */
    float loop_bw;   /* timing loop noise bandwidth, normalised to the symbol rate */
    float damping;   /* timing loop damping factor */
    float agc_alpha; /* AGC smoothing factor, applied once per input chunk (0..1] */
} drx_frontend_cfg;

void drx_frontend_cfg_default(drx_frontend_cfg* cfg, float sps, float rolloff);

drx_frontend* drx_frontend_new(const drx_frontend_cfg* cfg);
void drx_frontend_free(drx_frontend* fe);

/*
 * Process interleaved I/Q float input (n_in complex samples) and write at most
 * max_out symbols (interleaved I/Q) to out.
 *
 * Returns the number of symbols written. *n_consumed receives the number of
 * input samples consumed, which is less than n_in only when the output is
 * full. If strobe is not NULL, strobe[k] receives the absolute index of the
 * input sample nearest to symbol k (counted from the first sample ever
 * passed in), which lets callers map stream tags from samples to symbols.
 */
int drx_frontend_process(drx_frontend* fe,
                         const float* in,
                         int n_in,
                         float* out,
                         int max_out,
                         int* n_consumed,
                         uint64_t* strobe);

/*
 * The same processing split in two stages that can run on different
 * threads (one stage per thread, in order):
 *   drx_frontend_filter: frequency shift and matched filter, n samples in,
 *     n filtered samples out (y may be in);
 *   drx_frontend_timing: timing recovery and AGC on the filtered samples,
 *     same contract as drx_frontend_process.
 * Do not mix them with drx_frontend_process on the same object.
 */
void drx_frontend_filter(drx_frontend* fe, const float* in, int n, float* y);
int drx_frontend_timing(drx_frontend* fe,
                        const float* y,
                        int n_in,
                        float* out,
                        int max_out,
                        int* n_consumed,
                        uint64_t* strobe);

/* Frequency shift applied to the input before the matched filter, in
 * cycles per sample: the input is multiplied by exp(-j 2 pi freq n).
 * Takes effect on the next input sample (call it from the thread running
 * drx_frontend_filter when the stages are split). */
void drx_frontend_set_freq(drx_frontend* fe, double freq);

/* Current AGC gain and the integral part of the timing loop (symbol-period
 * fraction), mostly for monitoring. */
float drx_frontend_gain(const drx_frontend* fe);
float drx_frontend_timing_drift(const drx_frontend* fe);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_FRONTEND_H */
