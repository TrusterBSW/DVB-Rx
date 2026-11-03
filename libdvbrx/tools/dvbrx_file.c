/*
 * File-based DVB-S2 / DVB-S / DVB-C receiver: fc32 IQ (or symbols) in,
 * MPEG-TS out.
 *
 * usage: dvbrx_file [options] <in.fc32> <out.ts>
 *   -s SPS      input samples per symbol (runs the front-end); omit when
 *               the input is already one sample per symbol
 *   -r ROLLOFF  roll-off (default 0.2)
 *   -g GOLD     PL scrambling gold code (default 0), or rN for a root code
 *   -i ISI      input stream to output (default: all)
 *   -l RATE     DVB-S (first generation) instead of DVB-S2: "auto", "1/2",
 *               "2/3", "3/4", "5/6" or "7/8"
 *   -c QAM      DVB-C instead of DVB-S2: "auto", 16, 32, 64, 128 or 256
 *               (give -r 0.15)
 *   -T BW       DVB-T, channel bandwidth in MHz (8, 7, 6, 5): the input is
 *               complex samples at the -S rate
 *   -2 BW       DVB-T2, channel bandwidth in MHz; -i selects the PLP
 *   -S RATE     DVB-T / DVB-T2 input sample rate in Hz (default: the elementary rate,
 *               8/7 of the bandwidth)
 *   -n EsN0     test only: add Gaussian noise to symbol input, in dB
 *   -f FREQ     test only: add a carrier offset to symbol input, cycles/symbol
 */
#include "dvbrx/dvbc.h"
#include "dvbrx/dvbs.h"
#include "dvbrx/dvbt.h"
#include "dvbrx/dvbt2.h"
#include "dvbrx/frontend.h"
#include "dvbrx/pl.h"
#include "dvbrx/rx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void on_ts(void* ctx, int isi, const uint8_t* ts, int len)
{
    (void)isi;
    fwrite(ts, 1, len, (FILE*)ctx);
}

static void on_ts_dvbs(void* ctx, const uint8_t* ts, int len)
{
    fwrite(ts, 1, len, (FILE*)ctx);
}

/* "auto", "1/2", ... "7/8" -> enum drx_dvbs_rate, -1 if invalid */
static int parse_dvbs_rate(const char* s)
{
    static const char* const names[] = { "auto", "1/2", "2/3", "3/4", "5/6", "7/8" };
    for (int i = 0; i < 6; i++)
        if (!strcmp(s, names[i]))
            return i;
    return -1;
}

static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

int main(int argc, char** argv)
{
    float sps = 0, rolloff = 0.2f, esn0 = NAN, freq = 0;
    int dvbs_rate = -1; /* -1: DVB-S2 */
    int qam = -1;       /* -1: not DVB-C */
    double dvbt_bw = 0, dvbt2_bw = 0, rate = 0;
    drx_rx_cfg cfg;
    drx_rx_cfg_default(&cfg);
    int opt;
    while ((opt = getopt(argc, argv, "s:r:g:i:n:f:l:c:T:S:2:")) != -1) {
        switch (opt) {
        case 's': sps = (float)atof(optarg); break;
        case 'r': rolloff = (float)atof(optarg); break;
        case 'g': cfg.gold_code = drx_pls_parse(optarg); break;
        case 'i': cfg.isi = atoi(optarg); break;
        case 'n': esn0 = (float)atof(optarg); break;
        case 'f': freq = (float)atof(optarg); break;
        case 'l':
            if ((dvbs_rate = parse_dvbs_rate(optarg)) < 0)
                return 2;
            break;
        case 'T': dvbt_bw = atof(optarg) * 1e6; break;
        case '2': dvbt2_bw = atof(optarg) * 1e6; break;
        case 'S': rate = atof(optarg); break;
        case 'c': qam = strcmp(optarg, "auto") ? atoi(optarg) : 0; break;
        default: return 2;
        }
    }
    if (argc - optind != 2) {
        fprintf(stderr, "usage: %s [-s sps] [-r rolloff] [-g gold] [-i isi] [-n EsN0] "
                        "[-f freq] <in.fc32> <out.ts>\n", argv[0]);
        return 2;
    }
    FILE* in = fopen(argv[optind], "rb");
    FILE* out = fopen(argv[optind + 1], "wb");
    if (!in || !out) {
        perror("open");
        return 1;
    }

    drx_rx* rx = drx_rx_new(&cfg, on_ts, out);
    drx_dvbs* dvbs = dvbs_rate >= 0 ? drx_dvbs_new((enum drx_dvbs_rate)dvbs_rate, 0) : NULL;
    drx_dvbc* dvbc = NULL;
    if (qam >= 0 && !(dvbc = drx_dvbc_new((enum drx_qam)qam))) {
        fprintf(stderr, "bad QAM size %d\n", qam);
        return 2;
    }
    drx_dvbt* dvbt = NULL;
    if (dvbt_bw > 0 && !(dvbt = drx_dvbt_new(rate > 0 ? rate : dvbt_bw * 8 / 7, dvbt_bw, 0))) {
        fprintf(stderr, "bad DVB-T parameters\n");
        return 2;
    }
    drx_dvbt2* dvbt2 = NULL;
    if (dvbt2_bw > 0 && !(dvbt2 = drx_dvbt2_new(rate > 0 ? rate : dvbt2_bw * 8 / 7, dvbt2_bw,
                                                 cfg.isi >= 0 ? cfg.isi : -1))) {
        fprintf(stderr, "bad DVB-T2 parameters\n");
        return 2;
    }
    drx_frontend* fe = NULL;
    if (sps > 0) {
        drx_frontend_cfg fc;
        drx_frontend_cfg_default(&fc, sps, rolloff);
        fe = drx_frontend_new(&fc);
    }
    if (!rx || (sps > 0 && !fe))
        return 1;

    enum { CHUNK = 1 << 16 };
    float* buf = malloc(sizeof(float) * 2 * CHUNK);
    float* sym = malloc(sizeof(float) * 2 * (CHUNK + 1024));
    const double sigma = isnan(esn0) ? 0 : sqrt(pow(10, -esn0 / 10) / 2);
    double phase = 0;
    uint64_t nsym = 0;
    double proc = 0;
    size_t n;
    srand(5);
    while ((n = fread(buf, 8, CHUNK, in)) > 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        const float* s = buf;
        int ns = (int)n;
        if (fe) {
            int pos = 0;
            ns = 0;
            while (pos < (int)n) {
                int used;
                ns += drx_frontend_process(fe, buf + 2 * pos, (int)n - pos, sym + 2 * ns,
                                           CHUNK + 1024 - ns, &used, NULL);
                pos += used;
            }
            s = sym;
        } else if (sigma > 0 || freq != 0) {
            for (size_t k = 0; k < n; k++) {
                float c = (float)cos(phase), si = (float)sin(phase);
                float x = buf[2 * k], y = buf[2 * k + 1];
                buf[2 * k] = x * c - y * si + (float)(sigma * gauss());
                buf[2 * k + 1] = x * si + y * c + (float)(sigma * gauss());
                phase += 2 * M_PI * freq;
            }
        }
        if (!fe && (sigma > 0 || freq != 0)) {
            /* impairments are not part of the processing time */
            clock_gettime(CLOCK_MONOTONIC, &t0);
        }
        if (dvbt2)
            drx_dvbt2_process(dvbt2, s, ns, on_ts_dvbs, out);
        else if (dvbt)
            drx_dvbt_process(dvbt, s, ns, on_ts_dvbs, out);
        else if (dvbs)
            drx_dvbs_process(dvbs, s, ns, on_ts_dvbs, out);
        else if (dvbc)
            drx_dvbc_process(dvbc, s, ns, on_ts_dvbs, out);
        else
            drx_rx_symbols(rx, s, ns);
        nsym += ns;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        proc += (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
    }
    drx_rx_flush(rx);

    if (dvbs)
        drx_dvbs_flush(dvbs);
    if (dvbt2) {
        drx_dvbt2_flush(dvbt2);
        drx_dvbt2_stats ts;
        drx_dvbt2_get_stats(dvbt2, &ts);
        fprintf(stderr,
                "%llu samples in %.2f s (%.1f Msps); DVB-T2 %s, %dK GI %d/%d%s PP%d, %d PLP, "
                "PLP %d %d-QAM%s %s %s, MER %.2f dB, freq %.6f cyc/sample, clock %.1f ppm\n"
                "%llu frames (%llu L1 errors), FEC blocks %llu (%llu failed), ts %llu packets, "
                "%llu crc errors, %llu resyncs\n",
                (unsigned long long)nsym, proc, nsym / proc / 1e6,
                ts.locked ? "locked" : "not locked", ts.fft / 1024, ts.guard_num, ts.guard_den,
                ts.extended ? " ext" : "", ts.pilot_pattern, ts.num_plp, ts.plp_id, ts.qam,
                ts.rotated ? " rot" : "", ts.rate ? ts.rate : "-", ts.short_frame ? "16K" : "64K",
                ts.snr_db, ts.freq, ts.clock_ppm, (unsigned long long)ts.frames,
                (unsigned long long)ts.l1_errors, (unsigned long long)ts.fec_blocks,
                (unsigned long long)ts.fec_failed, (unsigned long long)ts.packets,
                (unsigned long long)ts.crc_errors, (unsigned long long)ts.resyncs);
        fclose(out);
        drx_dvbt2_free(dvbt2);
        drx_rx_free(rx);
        drx_frontend_free(fe);
        return 0;
    }
    if (dvbt) {
        static const char* const names[] = { "-", "1/2", "2/3", "3/4", "5/6", "7/8" };
        drx_dvbt_flush(dvbt);
        drx_dvbt_stats ts;
        drx_dvbt_get_stats(dvbt, &ts);
        fprintf(stderr,
                "%llu samples in %.2f s (%.1f Msps); DVB-T %s, %dK GI 1/%d, %d-QAM, rate %s, "
                "hierarchy %d, MER %.2f dB, freq %.6f cyc/sample, clock %.1f ppm\n"
                "%llu OFDM symbols, ts %llu packets, RS fixed %llu bytes, %llu packets "
                "uncorrectable, %llu resyncs\n",
                (unsigned long long)nsym, proc, nsym / proc / 1e6, ts.locked ? "locked" : "not locked",
                ts.fft / 1024, ts.guard, ts.qam, names[ts.rate], ts.hierarchy, ts.snr_db, ts.freq,
                ts.clock_ppm, (unsigned long long)ts.symbols, (unsigned long long)ts.packets,
                (unsigned long long)ts.rs_corrected, (unsigned long long)ts.rs_failed,
                (unsigned long long)ts.resyncs);
        fclose(out);
        drx_dvbt_free(dvbt);
        drx_rx_free(rx);
        drx_frontend_free(fe);
        return 0;
    }
    if (dvbc) {
        drx_dvbc_stats cs;
        drx_dvbc_get_stats(dvbc, &cs);
        fprintf(stderr,
                "%llu symbols in %.2f s (%.1f Msym/s); DVB-C %s, %d-QAM, MER %.2f dB, "
                "freq %.5f cyc/sym\nts %llu packets, RS fixed %llu bytes, %llu packets "
                "uncorrectable, %llu resyncs\n",
                (unsigned long long)nsym, proc, nsym / proc / 1e6,
                cs.locked ? "locked" : "not locked", cs.qam, cs.snr_db, cs.freq,
                (unsigned long long)cs.packets, (unsigned long long)cs.rs_corrected,
                (unsigned long long)cs.rs_failed, (unsigned long long)cs.resyncs);
        fclose(out);
        drx_dvbc_free(dvbc);
        drx_rx_free(rx);
        drx_frontend_free(fe);
        return 0;
    }
    if (dvbs) {
        static const char* const names[] = { "-", "1/2", "2/3", "3/4", "5/6", "7/8" };
        drx_dvbs_stats ds;
        drx_dvbs_get_stats(dvbs, &ds);
        fprintf(stderr,
                "%llu symbols in %.2f s (%.1f Msym/s); DVB-S %s, rate %s, SNR %.2f dB, "
                "freq %.5f cyc/sym\nts %llu packets, RS fixed %llu bytes, %llu packets "
                "uncorrectable, %llu resyncs\n",
                (unsigned long long)nsym, proc, nsym / proc / 1e6,
                ds.locked ? "locked" : "not locked", names[ds.rate], ds.snr_db, ds.freq,
                (unsigned long long)ds.packets, (unsigned long long)ds.rs_corrected,
                (unsigned long long)ds.rs_failed, (unsigned long long)ds.resyncs);
        fclose(out);
        drx_dvbs_free(dvbs);
        drx_rx_free(rx);
        drx_frontend_free(fe);
        return 0;
    }
    drx_rx_stats st;
    drx_rx_get_stats(rx, &st);
    fprintf(stderr,
            "%llu symbols in %.2f s (%.1f Msym/s); locked %d, %s %s%s%s, "
            "SNR %.2f dB, freq %.5f cyc/sym\n"
            "plframes %llu, ldpc batches %llu (failed %llu, avg %.1f it), bch failed %llu "
            "(fixed %llu bits), ts %llu packets, %llu crc errors, %llu bad bbheaders\n",
            (unsigned long long)nsym, proc, nsym / proc / 1e6, st.locked,
            st.constellation ? st.constellation : "-", st.rate ? st.rate : "-",
            st.short_frame ? " short" : "", st.pilots ? " pilots" : "", st.snr_db, st.freq,
            (unsigned long long)st.plframes, (unsigned long long)st.ldpc_batches,
            (unsigned long long)st.ldpc_failed_batches, st.ldpc_avg_iter,
            (unsigned long long)st.bch_failed, (unsigned long long)st.bch_corrected_bits,
            (unsigned long long)st.ts_packets, (unsigned long long)st.ts_crc_errors,
            (unsigned long long)st.bb_bad_headers);
    fclose(out);
    drx_rx_free(rx);
    drx_frontend_free(fe);
    return 0;
}
