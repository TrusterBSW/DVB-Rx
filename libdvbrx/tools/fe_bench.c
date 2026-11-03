/*
 * Front-end throughput bench: runs an fc32 IQ file through the front-end as
 * fast as possible and optionally writes the symbols out (fc32).
 *
 * usage: fe_bench <in.fc32> <sps> <rolloff> [out.fc32] [rrc_delay] [loop_bw]
 */
#include "dvbrx/frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <in.fc32> <sps> <rolloff> [out.fc32] [rrc_delay]\n",
                argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 8;
    rewind(f);
    float* in = malloc((size_t)n * 8);
    if (!in || fread(in, 8, n, f) != (size_t)n) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(f);

    drx_frontend_cfg cfg;
    drx_frontend_cfg_default(&cfg, (float)atof(argv[2]), (float)atof(argv[3]));
    if (argc > 5)
        cfg.rrc_delay = atoi(argv[5]);
    if (argc > 6)
        cfg.loop_bw = (float)atof(argv[6]);
    drx_frontend* fe = drx_frontend_new(&cfg);
    if (!fe) {
        fprintf(stderr, "bad config\n");
        return 1;
    }

    long max_sym = (long)(n / cfg.sps) + 1024;
    float* out = malloc((size_t)max_sym * 8);
    long pos = 0, nsym = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (pos < n) {
        int blk = n - pos < 32768 ? (int)(n - pos) : 32768;
        int used;
        nsym += drx_frontend_process(fe, in + 2 * pos, blk, out + 2 * nsym,
                                     (int)(max_sym - nsym), &used, NULL);
        pos += used;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);

    printf("%ld samples -> %ld symbols in %.3f s: %.1f Msym/s (%.1f Msps in), "
           "gain %.3f, drift %.2e\n",
           n, nsym, dt, nsym / dt / 1e6, n / dt / 1e6, drx_frontend_gain(fe),
           drx_frontend_timing_drift(fe));

    if (argc > 4) {
        FILE* o = fopen(argv[4], "wb");
        if (!o || fwrite(out, 8, nsym, o) != (size_t)nsym) {
            perror(argv[4]);
            return 1;
        }
        fclose(o);
    }
    drx_frontend_free(fe);
    free(in);
    free(out);
    return 0;
}
