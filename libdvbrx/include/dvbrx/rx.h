/*
 * DVB-S2 receiver back half: symbols (one per symbol, after the front-end)
 * in, MPEG-TS out. Chains PL sync, demapper, LDPC, BCH and the BB layer,
 * decoding frames of the same MODCOD in batches.
 */
#ifndef DVBRX_RX_H
#define DVBRX_RX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRX_ISI_ALL (-1)   /* output every input stream (callback gets the ISI) */
#define DRX_ISI_FIRST (-2) /* output the first stream seen, like a tuner card */

/* Called with whole TS packets of input stream isi. */
typedef void (*drx_ts_cb)(void* ctx, int isi, const uint8_t* ts, int len);

typedef struct {
    int gold_code; /* PL scrambling sequence index */
    int max_batch; /* frames per LDPC batch, 0 = SIMD width */
    int ldpc_iter; /* LDPC iteration limit */
    int isi;       /* input stream to output, DRX_ISI_ALL or DRX_ISI_FIRST */
} drx_rx_cfg;

typedef struct {
    uint64_t plframes;
    uint64_t ldpc_batches, ldpc_failed_batches;
    uint64_t bch_failed, bch_corrected_bits;
    uint64_t ts_packets, ts_crc_errors;
    uint64_t bb_bad_headers;
    int locked;
    int pls;                   /* PLS code of the last frame, pilot bit cleared */
    const char* constellation; /* of the last frame, NULL before the first one */
    const char* rate;
    int short_frame, pilots;
    float snr_db, freq;
    float ldpc_avg_iter;
} drx_rx_stats;

typedef struct drx_rx drx_rx;

void drx_rx_cfg_default(drx_rx_cfg* cfg);
drx_rx* drx_rx_new(const drx_rx_cfg* cfg, drx_ts_cb cb, void* ctx);
void drx_rx_free(drx_rx* rx);

/* Feed n symbols (interleaved I/Q). */
void drx_rx_symbols(drx_rx* rx, const float* sym, int n);
/* Decode the frames waiting in an incomplete batch. */
void drx_rx_flush(drx_rx* rx);

void drx_rx_get_stats(const drx_rx* rx, drx_rx_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_RX_H */
