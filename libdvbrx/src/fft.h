/*
 * Complex FFT of a power-of-two size (internal): DVB-T / DVB-T2 OFDM
 * symbols (1K .. 32K) and spectral estimates.
 */
#ifndef DVBRX_FFT_H
#define DVBRX_FFT_H

#include <complex.h>

struct fft {
    int n, log2n;
    int r0;            /* first pass radix (4 or 8), 0: plain radix-2 (n < 64) */
    float complex* tw; /* radix-2: e^(-2 pi j k / n), k < n / 2 */
    int* rev;          /* radix-2: bit reversal permutation; else first pass blocks */
    float* tw4;        /* radix-4 passes: twiddles, expanded for vectors */
    float complex* tmp; /* work buffer of n values */
};

int fft_init(struct fft* f, int n);
void fft_free(struct fft* f);
/* In place: X[k] = sum x[t] e^(-2 pi j k t / n) (inverse: e^(+...), no 1/n).
 * Uses f's work buffer: one call at a time per struct fft. */
void fft_run(const struct fft* f, float complex* x, int inverse);

#endif
