/*
 * DVB-S2 physical layer: PLFRAME synchronisation, PLS decoding, PL
 * descrambling and carrier phase/frequency recovery. Symbols in, XFECFRAMEs
 * (data symbols, pilots removed, unit average energy) out.
 */
#ifndef DVBRX_PL_H
#define DVBRX_PL_H

#include "dvbrx/demap.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    drx_modcod modcod;
    int pilots;
    int nsym;        /* XFECFRAME symbols */
    float n0;        /* noise power per symbol, after normalisation */
    float snr_db;    /* Es/N0 measured on the known symbols */
    float freq;      /* carrier frequency estimate, cycles per symbol */
    int settled;     /* the frequency estimate has converged since lock */
    uint64_t index;  /* PLFRAME counter */
} drx_plframe;

typedef struct {
    uint64_t frames;  /* data PLFRAMEs output */
    uint64_t dummies; /* dummy PLFRAMEs */
    uint64_t lost;    /* loss of lock events */
    uint64_t skipped; /* symbols dropped while searching */
    int locked;
} drx_pl_stats;

typedef struct drx_pl drx_pl;

/* Called for each data PLFRAME; xfec holds info->nsym interleaved I/Q
 * symbols, valid during the call only. */
typedef void (*drx_pl_cb)(void* ctx, const drx_plframe* info, const float* xfec);

/* Optional: where to write the XFECFRAME symbols of the next frame, to
 * spare the caller a copy. Called with the ctx of drx_pl_process() once the
 * frame's MODCOD is known; returns room for 2 * nsym floats, or NULL for
 * the internal buffer. The buffer is filled, then passed to cb as xfec
 * unless the frame is dropped (bad SNR), in which case cb is not called. */
typedef float* (*drx_pl_out_cb)(void* ctx, const drx_modcod* modcod, int nsym);

/* Converts a PL scrambling "root" code (the x register state, as some
 * satellite listings give it) to the gold code index n. Returns -1 if the
 * value is not a valid root code. */
int drx_pls_root_to_gold(uint32_t root);

/* Parses "N" (gold code) or "rN" (root code). Returns -1 if invalid. */
int drx_pls_parse(const char* s);

/* gold_code: PL scrambling sequence index n (0 by default in DVB-S2). */
drx_pl* drx_pl_new(int gold_code);
void drx_pl_free(drx_pl* pl);

/* Sets the output buffer provider used by drx_pl_process() (NULL: none). */
void drx_pl_set_out_cb(drx_pl* pl, drx_pl_out_cb get);

/* Feed n symbols (interleaved I/Q). Complete PLFRAMEs go to cb. */
void drx_pl_process(drx_pl* pl, const float* sym, int n, drx_pl_cb cb, void* ctx);

/* The symbols fed from now on have been shifted by -delta cycles/symbol
 * (carrier frequency correction upstream). The frequency estimate is moved
 * accordingly when the first frame starting after this point comes up. */
void drx_pl_shift(drx_pl* pl, double delta);
/* Non-zero while a shift notified by drx_pl_shift() is not applied yet. */
int drx_pl_shift_pending(const drx_pl* pl);

/* Discontinuity in the input: drop the buffered symbols and search again. */
void drx_pl_reset(drx_pl* pl);

void drx_pl_get_stats(const drx_pl* pl, drx_pl_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_PL_H */
