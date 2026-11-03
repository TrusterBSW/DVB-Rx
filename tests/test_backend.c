/*
 * Receiver back-end test: XFECFRAME symbols (txvectors.py xfecframe.fc32)
 * plus Gaussian noise -> demapper -> LDPC -> BCH -> BB -> TS file.
 * Prints per-stage throughput.
 *
 * usage: test_backend <xfecframe.fc32> <constellation> <rate> <EsN0_dB> <out.ts>
 *                     [short] [estimate]
 *   estimate: use the decision-directed n0 estimate instead of the true n0
 */
#include "dvbrx/bb.h"
#include "dvbrx/demap.h"
#include "dvbrx/ldpc.h"

#include <math.h>
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

static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage: %s <xfecframe.fc32> <constellation> <rate> <EsN0_dB> <out.ts> "
                "[short] [estimate]\n",
                argv[0]);
        return 2;
    }
    int short_frame = 0, estimate = 0;
    for (int i = 6; i < argc; i++) {
        short_frame |= !strcmp(argv[i], "short");
        estimate |= !strcmp(argv[i], "estimate");
    }
    drx_fec_params p;
    if (drx_fec_lookup(short_frame, argv[3], &p))
        return 2;
    drx_demap* dm = drx_demap_new(argv[2], argv[3], short_frame);
    drx_ldpc* ld = drx_ldpc_new(&p, argv[3]);
    drx_bch* bc = drx_bch_new(&p);
    drx_bbdec* bb = drx_bbdec_new();
    if (!dm || !ld || !bc || !bb) {
        fprintf(stderr, "unsupported MODCOD\n");
        return 2;
    }
    /* XFECFRAME stride: whole slots (128APSK: 9258 symbols + 12 dummies) */
    const int nsym = drx_demap_symbols(dm), lanes = drx_ldpc_lanes();
    const int stride = (nsym + 89) / 90 * 90;

    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    const int nf = (int)(ftell(f) / (8L * stride));
    rewind(f);
    float* sym = malloc((size_t)nf * stride * 8);
    if (fread(sym, 8L * stride, nf, f) != (size_t)nf)
        return 1;
    fclose(f);

    const double n0 = pow(10, -atof(argv[4]) / 10);
    srand(3);
    for (size_t i = 0; i < (size_t)nf * stride * 2; i++)
        sym[i] += (float)(sqrt(n0 / 2) * gauss());

    int8_t* llr = malloc((size_t)nf * p.nldpc);
    uint8_t* cw = malloc((size_t)nf * p.kldpc / 8);
    uint8_t* ts = malloc(p.kbch / 8 + DRX_TS_PACKET_LEN);
    FILE* o = fopen(argv[5], "wb");

    double t0 = now();
    float n0_used = (float)n0;
    if (estimate)
        n0_used = drx_demap_estimate_n0(dm, sym, nsym);
    for (int k = 0; k < nf; k++)
        drx_demap_frame(dm, sym + (size_t)k * stride * 2, n0_used, llr + (size_t)k * p.nldpc);
    double t1 = now();
    int iters = 0, batches = 0, failed = 0;
    for (int f0 = 0; f0 < nf; f0 += lanes) {
        int n = nf - f0 < lanes ? nf - f0 : lanes;
        const int8_t* in[64];
        uint8_t* out[64];
        for (int k = 0; k < n; k++) {
            in[k] = llr + (size_t)(f0 + k) * p.nldpc;
            out[k] = cw + (size_t)(f0 + k) * p.kldpc / 8;
        }
        int it = drx_ldpc_decode(ld, in, out, n, 25);
        iters += it < 0 ? 25 : it;
        failed += it < 0;
        batches++;
    }
    double t2 = now();
    int bch_fail = 0, bch_fixed = 0;
    for (int k = 0; k < nf; k++) {
        int r = drx_bch_decode(bc, cw + (size_t)k * p.kldpc / 8);
        if (r < 0)
            bch_fail++;
        else
            bch_fixed += r;
    }
    double t3 = now();
    for (int k = 0; k < nf; k++) {
        uint8_t* fr = cw + (size_t)k * p.kldpc / 8;
        drx_bb_descramble(fr, p.kbch / 8);
        drx_bbheader h;
        int n = drx_bbdec_frame(bb, fr, p.kbch / 8, ts, &h);
        if (n > 0)
            fwrite(ts, 1, n, o);
    }
    double t4 = now();
    fclose(o);

    drx_bbdec_stats st;
    drx_bbdec_get_stats(bb, &st);
    printf("%s %s EsN0 %.1f dB (n0 %s %.4f vs %.4f): %d frames\n", argv[2], argv[3],
           atof(argv[4]), estimate ? "est" : "true", n0_used, n0, nf);
    printf("  ldpc: avg %.1f iterations, %d/%d batches not converged; bch: %d failed, "
           "%d bits fixed; ts: %llu packets, %llu crc errors\n",
           (double)iters / batches, failed, batches, bch_fail, bch_fixed,
           (unsigned long long)st.packets, (unsigned long long)st.crc_errors);
    printf("  frames/s: demap %.0f, ldpc %.0f, bch %.0f, bb %.0f\n", nf / (t1 - t0),
           nf / (t2 - t1), nf / (t3 - t2), nf / (t4 - t3));
    return st.crc_errors != 0;
}
