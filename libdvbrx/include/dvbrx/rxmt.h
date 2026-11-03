/*
 * Multithreaded receiver (DVB-S2, DVB-S, DVB-C, DVB-T, DVB-T2): complex baseband
 * samples in, MPEG-TS out.
 *
 * Threads: front-end (matched filter, timing) -> PL sync + demapper (forms
 * LDPC batches) -> a pool of LDPC decoders -> output (reordering, BCH, BB
 * layer, TS callback).
 */
#ifndef DVBRX_RXMT_H
#define DVBRX_RXMT_H

#include "dvbrx/frontend.h"
#include "dvbrx/rx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    drx_frontend_cfg fe;
    drx_rx_cfg rx;
    int dvbs;           /* 1: DVB-S (first generation) instead of DVB-S2 */
    int dvbs_rate;      /* enum drx_dvbs_rate, DRX_DVBS_AUTO to detect it */
    int dvbc;           /* 1: DVB-C instead of DVB-S2 (roll-off 0.15) */
    int dvbc_qam;       /* enum drx_qam, DRX_QAM_AUTO to detect it (else
                         * tried first, then the others) */
    int dvbc_inversion; /* enum fe_spectral_inversion: 0, 1, 2 auto */
    int dvbt;           /* 1: DVB-T (OFDM: the front-end and the fe settings
                         * are not used) */
    double sample_rate; /* DVB-T: input sample rate, Hz (at least 8/7 of the
                         * bandwidth) */
    double bandwidth;   /* DVB-T: channel bandwidth, Hz */
    int dvbt2;          /* 1: DVB-T2 (same settings as DVB-T; rx.isi selects
                         * the PLP, DRX_ISI_FIRST / DRX_ISI_ALL the first) */
    int ldpc_threads;   /* decoder threads (demapping and LDPC) */
    int demap_threads;  /* of which demap only: -1 auto (1 of 4, 2 of 5 or
                         * more: more than 3 LDPC decoders contend for the
                         * L3 cache), 0 demapping in the LDPC workers;
                         * >= 0: in addition to ldpc_threads */
    int max_latency_ms; /* send an incomplete LDPC batch after this long */
} drx_mt_cfg;

typedef struct drx_mt drx_mt;

void drx_mt_cfg_default(drx_mt_cfg* cfg, float sps, float rolloff);

/* cb runs on the output thread. */
drx_mt* drx_mt_new(const drx_mt_cfg* cfg, drx_ts_cb cb, void* ctx);

/*
 * Push n samples (interleaved I/Q). Blocks while the pipeline is full
 * unless nonblock is set, in which case the samples that do not fit are
 * dropped. Returns the number of samples accepted.
 */
int drx_mt_push(drx_mt* mt, const float* iq, int n, int nonblock);

/* Process everything pushed so far, then stop the threads. */
void drx_mt_finish(drx_mt* mt);
void drx_mt_free(drx_mt* mt);

void drx_mt_get_stats(drx_mt* mt, drx_rx_stats* s);
/* Samples dropped by non-blocking pushes. */
uint64_t drx_mt_dropped(drx_mt* mt);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_RXMT_H */
