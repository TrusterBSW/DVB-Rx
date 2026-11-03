/*
 * BCH test on txvectors.py codewords (bch.u8, one bit per byte):
 * clean codewords must decode with 0 corrections; up to t random errors
 * must be corrected; t+1 errors must not be miscorrected silently into a
 * different codeword too often. Also measures throughput.
 *
 * usage: test_bch <bch.u8> <rate> [short]
 */
#include "dvbrx/fec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bch.u8> <rate> [short]\n", argv[0]);
        return 2;
    }
    drx_fec_params p;
    if (drx_fec_lookup(argc > 3, argv[2], &p)) {
        fprintf(stderr, "unknown rate\n");
        return 2;
    }
    drx_bch* b = drx_bch_new(&p);
    FILE* f = fopen(argv[1], "rb");
    const int n = p.kldpc, nb = n / 8;
    uint8_t* bits = malloc(n);
    int nframes = 0, cap = 1024;
    uint8_t* cws = malloc((size_t)cap * nb);
    while (fread(bits, 1, n, f) == (size_t)n && nframes < cap) {
        uint8_t* cw = cws + (size_t)nframes++ * nb;
        for (int i = 0; i < nb; i++) {
            uint8_t v = 0;
            for (int j = 0; j < 8; j++)
                v = (uint8_t)(v << 1 | (bits[8 * i + j] & 1));
            cw[i] = v;
        }
    }
    printf("%d codewords, kbch %d nbch %d t %d\n", nframes, p.kbch, n, p.t);

    int fails = 0;
    uint8_t* w = malloc(nb);
    double t0 = now();
    for (int k = 0; k < nframes; k++) {
        memcpy(w, cws + (size_t)k * nb, nb);
        if (drx_bch_decode(b, w) != 0)
            fails++;
    }
    double dt = now() - t0;
    printf("clean: %d failures, %.0f frames/s\n", fails, nframes / dt);

    srand(1);
    for (int nerr = 1; nerr <= p.t + 1; nerr++) {
        int ok = 0, detected = 0, wrong = 0;
        t0 = now();
        for (int k = 0; k < nframes; k++) {
            const uint8_t* ref = cws + (size_t)k * nb;
            memcpy(w, ref, nb);
            for (int e = 0; e < nerr; e++) {
                int bit;
                do
                    bit = rand() % n;
                while ((w[bit / 8] ^ ref[bit / 8]) & (0x80 >> (bit % 8)));
                w[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
            }
            int r = drx_bch_decode(b, w);
            if (r < 0)
                detected++;
            else if (memcmp(w, ref, nb) == 0 && r == nerr)
                ok++;
            else
                wrong++;
        }
        dt = now() - t0;
        printf("%2d errors: %d corrected, %d detected uncorrectable, %d wrong "
               "(%.0f frames/s)\n",
               nerr, ok, detected, wrong, nframes / dt);
        if (nerr <= p.t && ok != nframes)
            fails++;
    }
    drx_bch_free(b);
    return fails != 0;
}
