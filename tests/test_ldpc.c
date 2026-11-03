/*
 * LDPC test on txvectors.py codewords: adds Gaussian noise as seen by one
 * QPSK component at the given Es/N0, decodes in batches and compares with
 * the BCH codewords. Reports the frame error rate and throughput.
 *
 * usage: test_ldpc <ldpc.u8> <bch.u8> <rate> <EsN0_dB> [short]
 */
#include "dvbrx/ldpc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

static uint8_t* load_bits(const char* path, int len, int* nframes)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *nframes = (int)(ftell(f) / len);
    rewind(f);
    uint8_t* b = malloc((size_t)*nframes * len);
    if (fread(b, len, *nframes, f) != (size_t)*nframes)
        return NULL;
    fclose(f);
    return b;
}

int main(int argc, char** argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <ldpc.u8> <bch.u8> <rate> <EsN0_dB> [short]\n", argv[0]);
        return 2;
    }
    drx_fec_params p;
    if (drx_fec_lookup(argc > 5, argv[3], &p))
        return 2;
    drx_ldpc* d = drx_ldpc_new(&p, argv[3]);
    if (!d) {
        fprintf(stderr, "no LDPC table\n");
        return 2;
    }
    int nf, nf2;
    uint8_t* cw = load_bits(argv[1], p.nldpc, &nf);
    uint8_t* ref = load_bits(argv[2], p.kldpc, &nf2);
    if (!cw || !ref || nf != nf2)
        return 1;

    const double esn0 = pow(10, atof(argv[4]) / 10), sigma2 = 1 / esn0;
    const int lanes = drx_ldpc_lanes();
    /* Same int8 LLR scale as the demapper will use: 2y/sigma^2, x2. */
    int8_t* llr = malloc((size_t)nf * p.nldpc);
    srand(7);
    for (size_t i = 0; i < (size_t)nf * p.nldpc; i++) {
        double y = (cw[i] ? -1.0 : 1.0) + sqrt(sigma2) * gauss();
        double l = 2 * 2 * y / sigma2;
        llr[i] = (int8_t)(l > 127 ? 127 : l < -127 ? -127 : lrint(l));
    }

    uint8_t* out = malloc((size_t)nf * p.kldpc / 8);
    int errors = 0, raw_errors = 0, batches = 0, iters = 0, failed = 0;
    double t0 = now();
    for (int f0 = 0; f0 < nf; f0 += lanes) {
        const int n = nf - f0 < lanes ? nf - f0 : lanes;
        const int8_t* in[64];
        uint8_t* o[64];
        for (int k = 0; k < n; k++) {
            in[k] = llr + (size_t)(f0 + k) * p.nldpc;
            o[k] = out + (size_t)(f0 + k) * p.kldpc / 8;
        }
        int it = drx_ldpc_decode(d, in, o, n, 25);
        batches++;
        if (it < 0) {
            failed++;
            iters += 25;
        } else {
            iters += it;
        }
    }
    double dt = now() - t0;

    for (int f = 0; f < nf; f++) {
        int bad = 0;
        for (int j = 0; j < p.kldpc; j++) {
            int b = (out[(size_t)f * p.kldpc / 8 + j / 8] >> (7 - j % 8)) & 1;
            bad += b != ref[(size_t)f * p.kldpc + j];
            raw_errors += (llr[(size_t)f * p.nldpc + j] < 0) != ref[(size_t)f * p.kldpc + j];
        }
        errors += bad != 0;
    }
    printf("%s EsN0 %.1f dB: %d frames, raw BER %.2e, frames in error %d, "
           "avg iterations %.1f (%d batches not converged), %.0f frames/s (%d lanes)\n",
           argv[3], atof(argv[4]), nf, (double)raw_errors / ((double)nf * p.kldpc), errors,
           (double)iters / batches, failed, nf / dt, lanes);
    drx_ldpc_free(d);
    return 0;
}
