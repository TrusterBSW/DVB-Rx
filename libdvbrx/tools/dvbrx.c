/*
 * dvbrx: real-time DVB receiver (S/S2/S2X, C, T, T2) for an SDR (USRP through
 * UHD, bladeRF through libbladeRF; see tools/sdr.h) or an IQ file; runs as the
 * dvbrxd@N service in DVB bridge mode.
 *
 * Standalone:  dvbrx (-u ARGS -f FREQ | -F FILE) -s SYMRATE [-o DEST] ...
 * DVB bridge:  dvbrx -d N (-u ARGS | -F FILE) [-L LNB] ...
 *   Serves /dev/dvb/adapterN through the dvbrx kernel module: tuning
 *   requests come from the applications (frequency = L-band IF, symbol
 *   rate, ISI, PLS gold code), the transport stream goes to the adapter's
 *   demux, LNB commands go to the LNB controller (-L).
 *
 * Options:
 *   -u ARGS     SDR: "[DRIVER:]ARGS", DRIVER "uhd" (default; ARGS e.g.
 *               "type=b200"), "bladerf" (ARGS: libbladeRF device string,
 *               "" for the first one) or "hackrf" (ARGS: serial number, ""
 *               for the first one; 20 Msps at most, one channel)
 *   -F FILE     fc32 IQ file (bridge mode: looped in real time, frequency
 *               ignored)
 *   -f HZ       centre frequency (standalone)
 *   -s SYMRATE  symbol rate (standalone)
 *   -S RATE     sample rate (standalone, default 2 x symbol rate); bridge
 *               mode: maximum sample rate (default 56e6)
 *   -G DB       receive gain (default 40), or "auto": first kept for a mean
 *               input level of -24 dBFS (lowered at once if the ADC clips),
 *               then, once the receiver locks, moved in 3 dB steps to the
 *               gain giving the best SNR (the other channels of a cable or
 *               an antenna can overload the front-end well before the
 *               wanted one fills the ADC); "auto:MAX" caps it at MAX dB
 *   -B HZ       analog baseband filter bandwidth (default: the occupied
 *               bandwidth of the channel + 15 %, or the whole capture for
 *               several channels; 0: UHD's choice)
 *   -A ANT      receive antenna: UHD "TX/RX" or "RX2" (default: UHD's),
 *               bladeRF "RX1" (default) or "RX2"; HackRF: one input (its
 *               port power is always off)
 *   -C PATH     bridge mode: control socket for dvbrx-web (default
 *               /run/dvbrx/dvbrx-N.sock, "" for none), see doc/dvbrx-web-api.md
 *   -P DB       input power estimate: dBm at 0 dBFS and 0 dB gain (default 0)
 *   -M HZ       master clock rate (UHD; default: chosen by UHD)
 *   -r ROLLOFF  roll-off (standalone, default 0.2)
 *   -D SYMBOLS  matched filter half-length (default: from the roll-off)
 *   -g GOLD     PL scrambling gold code (standalone, default 0), rN: root code
 *   -l RATE     standalone: DVB-S (first generation) instead of DVB-S2, code
 *               rate "auto", "1/2", "2/3", "3/4", "5/6" or "7/8"
 *   -c QAM      standalone: DVB-C instead of DVB-S2, "auto", 16, 32, 64, 128
 *               or 256 (roll-off 0.15)
 *   -T MHZ      standalone: DVB-T, channel bandwidth (8, 7, 6, 5); no -s
 *   -2 MHZ      standalone: DVB-T2, channel bandwidth; -i selects the PLP
 *   -i ISI      input stream (standalone; default: all, UDP port + ISI)
 *   -o DEST     standalone output: "-" (stdout, default), a file, or
 *               udp://HOST:PORT
 *   -d N        DVB bridge on /dev/dvbrxN
 *   -L LNB      LNB controller: "none" (default), "serial:/dev/ttyACM0" or
 *               "udp:HOST:PORT" (text protocol, see doc/lnb-protocol.md)
 *   -t N        LDPC threads (default 2)
 *   -R          standalone file source: play at the sample rate, in a loop
 */
#include "dvbrx/chan.h"
#include "dvbrx/dvbc.h"
#include "dvbrx/dvbs.h"
#include "dvbrx/dvbt.h"
#include "dvbrx/pl.h"
#include "dvbrx/rxmt.h"
#include "dvbrx_ioctl.h"
#include "psi.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <linux/dvb/frontend.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "sdr.h"

static volatile sig_atomic_t stop;
static const char* ctl_path = "default"; /* -C: control socket, "" none */
static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

/* ---- radio: a sample source feeding the receiver from its own thread --------- */

struct radio {
    const struct sdr_driver* drv; /* NULL: file source */
    const char* dev_args;
    const char* file;
    int file_loop;     /* restart the file at its end */
    int file_realtime; /* pace the file at the sample rate, drop when late */
    double gain, mcr, gain_max;
    int gain_auto; /* -G auto: gain from the measured level */
    double gain_cap; /* -G auto:MAX, 0: none */
    double bw_opt;   /* -B: < 0 auto, 0 the driver's, > 0 fixed */
    double bw_need;  /* auto: bandwidth to pass (channel or capture), 0: rate */
    double bw_set;   /* analog bandwidth obtained */
    volatile int agc_state; /* 0 level AGC, 1 best-SNR search, 2 best gain held */
    const char* antenna;

    /* current run */
    double freq, rate;
    drx_mt* mt;
    /* when set, samples go there instead of mt (several channels) */
    void (*sink)(void* ctx, const float* iq, int n);
    void* sink_ctx;
    pthread_t thread;
    volatile int running;
    int started;
    uint64_t samples, overflows, filled;
    /* input level, measured on the radio thread: mean power (dBFS) and
     * clipped samples (|I| or |Q| near full scale) over the last window */
    volatile float level_dbfs, clip_ppm;
    volatile int eof;

    struct sdr* dev; /* open SDR, NULL until the first tuning */
    struct sdr_info info;
};

static void radio_deliver(struct radio* r, const float* iq, int n, int nonblock)
{
    if (r->sink)
        r->sink(r->sink_ctx, iq, n);
    else
        drx_mt_push(r->mt, iq, n, nonblock);
}

static void* file_thread(void* arg)
{
    struct radio* r = arg;
    FILE* f = fopen(r->file, "rb");
    if (!f) {
        perror(r->file);
        r->eof = 1;
        return NULL;
    }
    enum { N = 65536 };
    float* buf = malloc(sizeof(float) * 2 * N);
    const double t0 = now();
    uint64_t total = 0;
    while (r->running) {
        size_t n = fread(buf, 8, N, f);
        if (n == 0) {
            if (!r->file_loop)
                break;
            rewind(f);
            continue;
        }
        if (r->file_realtime) {
            double wait = t0 + (total + n) / r->rate - now();
            if (wait > 0)
                usleep((useconds_t)(wait * 1e6));
        }
        radio_deliver(r, buf, (int)n, r->file_realtime);
        total += n;
        r->samples = total;
    }
    r->eof = 1;
    free(buf);
    fclose(f);
    return NULL;
}

/* -u [DRIVER:]ARGS: the SDR driver and its device arguments (UHD without a
 * driver name, as before). */
static int radio_select(struct radio* r, const char* spec)
{
    static const struct sdr_driver* const drivers[] = {
#ifdef HAVE_UHD
        &sdr_uhd,
#endif
#ifdef HAVE_BLADERF
        &sdr_bladerf,
#endif
#ifdef HAVE_HACKRF
        &sdr_hackrf,
#endif
        NULL,
    };
    static const char* const known[] = { "uhd", "bladerf", "hackrf" };
    const char* colon = strchr(spec, ':');
    const size_t len = colon ? (size_t)(colon - spec) : 0;
    const char* want = "uhd";
    const char* args = spec;
    for (size_t i = 0; colon && i < sizeof(known) / sizeof(known[0]); i++)
        if (len == strlen(known[i]) && !strncmp(spec, known[i], len)) {
            want = known[i];
            args = colon + 1;
        }
    for (int i = 0; drivers[i]; i++)
        if (!strcmp(drivers[i]->prefix, want)) {
            r->drv = drivers[i];
            r->dev_args = args;
            return 0;
        }
    fprintf(stderr, "built without %s support\n", want);
    return -1;
}

static int radio_open(struct radio* r)
{
    if (r->dev)
        return 0;
    if (!r->drv)
        return -1;
    r->dev = r->drv->open(r->dev_args, r->mcr, &r->info);
    if (!r->dev)
        return -1;
    r->gain_max = r->info.gain_max;
    return 0;
}

/* Input level and automatic gain (-G auto). The level is the mean power
 * over windows of LEVEL_WIN seconds; the gain keeps it near AGC_TARGET
 * (dBFS: OFDM and QAM peaks stay ~10 dB below full scale), within
 * AGC_HYST, and drops at once when the ADC clips. A change is only made
 * every other window (the level after a change is not representative). */
#define LEVEL_WIN 0.5
#define AGC_TARGET -24.0
#define AGC_HYST 4.0
#define AGC_CLIP 1e-5 /* fraction of clipped samples */

struct level {
    double pow;
    uint64_t n, clip;
    int skip;
    /* best-SNR search (single receiver): 0 level AGC, 1 searching, 2 done */
    int opt, stable, dir, tried_down, unlocked;
    double best_g, best_snr, acc, base_dbfs;
    int nacc;
};

#define OPT_STEP 3.0   /* dB */
#define OPT_WINDOWS 2  /* level windows averaged per gain */
#define OPT_BETTER 0.3 /* dB of SNR to call a gain better */

static void radio_level(struct radio* r, struct level* lv, const float* iq, int n)
{
    double p = 0;
    uint64_t c = 0;
    for (int i = 0; i < 2 * n; i++) {
        const float v = iq[i];
        p += (double)v * v;
        c += v > 0.95f || v < -0.95f;
    }
    lv->pow += p;
    lv->clip += c;
    lv->n += n;
    if (lv->n < LEVEL_WIN * r->rate)
        return;
    const double dbfs = 10 * log10(lv->pow / lv->n + 1e-20);
    const double clip = (double)lv->clip / (2.0 * lv->n);
    r->level_dbfs = (float)dbfs;
    r->clip_ppm = (float)(clip * 1e6);
    lv->pow = 0;
    lv->n = lv->clip = 0;
    if (!r->gain_auto || r->file || !r->dev)
        return;
    if (lv->skip) {
        lv->skip = 0;
        return;
    }
    const double gmax = r->gain_cap > 0 ? fmin(r->gain_cap, r->gain_max) : r->gain_max;
    double g = r->gain;
    /* receiver state, for the best-SNR search (one receiver on the radio) */
    int locked = 0;
    float snr = 0;
    if (r->mt && !r->sink) {
        drx_rx_stats st;
        drx_mt_get_stats(r->mt, &st);
        locked = st.locked && st.snr_db > 0;
        snr = st.snr_db;
    }
    if (clip > AGC_CLIP) {
        g -= clip > 1e-2 ? 10 : 6; /* heavy clipping: the level is not measurable */
        lv->opt = 0;
        lv->stable = 0;
    } else if (lv->opt == 0) {
        if (dbfs < AGC_TARGET - AGC_HYST || dbfs > AGC_TARGET + AGC_HYST) {
            g += fmax(-10, fmin(10, AGC_TARGET - dbfs));
            lv->stable = 0;
        } else if (++lv->stable >= 2 && locked) {
            /* level reached and locked: look for the best SNR around */
            lv->opt = 1;
            lv->best_g = g;
            lv->best_snr = -1e9;
            lv->dir = 1;
            lv->tried_down = 0;
            lv->acc = 0;
            lv->nacc = 0;
        }
    } else if (lv->opt == 1) {
        if (!locked) { /* lost while searching: back to the best known, and the level AGC */
            lv->opt = 0;
            lv->stable = 0;
            if (lv->best_snr > -1e9)
                g = lv->best_g;
        } else {
            lv->acc += snr;
            if (++lv->nacc >= OPT_WINDOWS) {
                const double s = lv->acc / lv->nacc;
                /* the next gain to try, if any (the gain may go below 0 dB) */
                const double gmin = r->info.gain_min;
                double next = 0;
                int more = 0;
                fprintf(stderr, "AGC: gain %.0f dB: SNR %.2f dB\n", g, s);
                if (s > lv->best_snr + OPT_BETTER) {
                    lv->best_snr = s;
                    lv->best_g = g;
                    next = g + lv->dir * OPT_STEP;
                    more = 1;
                } else if (lv->dir > 0 && !lv->tried_down) {
                    lv->dir = -1;
                    lv->tried_down = 1;
                    next = lv->best_g - OPT_STEP;
                    more = 1;
                }
                if (!more || next < gmin || next > gmax) {
                    more = 0;
                    if (lv->dir > 0 && !lv->tried_down) {
                        lv->dir = -1;
                        lv->tried_down = 1;
                        next = lv->best_g - OPT_STEP;
                        more = next >= gmin && next <= gmax;
                    }
                }
                if (!more) {
                    g = lv->best_g;
                    lv->opt = 2;
                    lv->unlocked = 0;
                    lv->base_dbfs = dbfs + (lv->best_g - r->gain);
                    fprintf(stderr, "AGC: best SNR %.2f dB at %.0f dB\n", lv->best_snr, lv->best_g);
                } else {
                    g = next;
                }
                lv->acc = 0;
                lv->nacc = 0;
            }
        }
    } else { /* done: start over on a lasting loss of lock or a level change */
        lv->unlocked = locked ? 0 : lv->unlocked + 1;
        if (lv->unlocked >= 4 || fabs(dbfs - lv->base_dbfs) > 8) {
            lv->opt = 0;
            lv->stable = 0;
        }
    }
    r->agc_state = lv->opt;
    g = fmax(r->info.gain_min, fmin(gmax, round(g)));
    if (g != r->gain) {
        g = r->drv->set_gain(r->dev, g);
        fprintf(stderr, "AGC: level %.1f dBFS, clipping %.0f ppm: gain %.0f -> %.0f dB\n", dbfs,
                clip * 1e6, r->gain, g);
        r->gain = g;
        lv->skip = 1;
    }
}

/* Samples lost in an overflow are replaced by as many zeros (up to this
 * long): the receivers keep their timing (an OFDM receiver loses a few
 * symbols instead of its frame synchronisation). */
#define MAX_FILL 0.25 /* seconds */

static void radio_fill(struct radio* r, double lost)
{
    static const float zeros[2 * 8192];
    uint64_t left = (uint64_t)llround(lost);
    while (left && r->running) {
        const int n = left < 8192 ? (int)left : 8192;
        radio_deliver(r, zeros, n, 1);
        left -= n;
    }
}

static void* sdr_thread(void* arg)
{
    struct radio* r = arg;
    const struct sdr_driver* d = r->drv;
    enum { N = 65536 };
    float* buf = malloc(sizeof(float) * 2 * N);
    d->start(r->dev);
    double t_next = -1; /* device time of the next sample expected */
    struct level lv = { 0 };
    while (r->running) {
        int got = 0;
        while (got < N && r->running) {
            double t = -1;
            enum sdr_status st = SDR_OK;
            const int n = d->read(r->dev, buf + 2 * got, N - got, 1.0, &t, &st);
            if (n > 0 && t >= 0) {
                const double lost = t_next < 0 ? 0 : (t - t_next) * r->rate;
                if (lost >= 1 && lost < MAX_FILL * r->rate) {
                    radio_deliver(r, buf, got, 1);
                    radio_fill(r, lost);
                    memmove(buf, buf + 2 * got, sizeof(float) * 2 * n);
                    got = 0;
                    r->filled += (uint64_t)llround(lost);
                }
                t_next = t + n / r->rate;
            }
            if (st == SDR_OVERFLOW) {
                r->overflows++;
            } else if (st == SDR_TIMEOUT) {
                /* After an overflow the B2xx may stop streaming: restart. */
                fprintf(stderr, "%s: receive timeout, restarting the stream\n", r->info.name);
                d->stop(r->dev);
                d->start(r->dev);
            }
            got += n;
        }
        /* Never block the radio: drop samples if the receiver is behind. */
        radio_level(r, &lv, buf, got);
        radio_deliver(r, buf, got, 1);
        r->samples += got;
    }
    d->stop(r->dev);
    free(buf);
    return NULL;
}

/* Analog bandwidth within the driver's range. */
static double radio_bw_clamp(const struct radio* r, double bw)
{
    return bw < r->info.bw_min ? r->info.bw_min : bw > r->info.bw_max ? r->info.bw_max : bw;
}

/* Tune the source; returns the sample rate actually obtained (the SDR may
 * round it, e.g. to its master clock divided by an integer), or -1. */
static double radio_configure(struct radio* r, double freq, double rate)
{
    r->freq = freq;
    r->rate = rate;
    if (r->file)
        return rate;
    if (radio_open(r)) {
        if (!r->drv)
            fprintf(stderr, "no SDR driver\n");
        return -1;
    }
    const struct sdr_driver* d = r->drv;
    const double actual = d->set_rate(r->dev, rate);
    r->gain = d->set_gain(r->dev, r->gain);
    /* Analog filter: the low-pass ahead of the ADC. Narrow, it keeps the
     * neighbouring channels (often stronger on a cable) out of the ADC
     * range; the digital filters only act after it. */
    if (r->bw_opt != 0) {
        const double bw = r->bw_opt > 0 ? r->bw_opt : r->bw_need > 0 ? 1.15 * r->bw_need : rate;
        r->bw_set = d->set_bandwidth(r->dev, radio_bw_clamp(r, bw));
    }
    if (r->antenna && d->set_antenna(r->dev, r->antenna))
        fprintf(stderr, "%s: cannot select antenna \"%s\"\n", r->info.name, r->antenna);
    const double rf = d->set_freq(r->dev, freq);
    fprintf(stderr, "%s: %.3f MHz, %.6f Msps (asked %.6f), gain %.1f dB, analog bandwidth %.2f MHz\n",
            r->info.name, rf / 1e6, actual / 1e6, rate / 1e6, r->gain, r->bw_set / 1e6);
    r->rate = actual;
    return actual;
}

/* Moves the centre frequency, the sample rate and the stream unchanged. */
static void radio_retune(struct radio* r, double freq)
{
    r->freq = freq;
    if (r->file || !r->dev)
        return;
    const double rf = r->drv->set_freq(r->dev, freq);
    fprintf(stderr, "%s: retuned to %.3f MHz\n", r->info.name, rf / 1e6);
}

/* Analog filter for the channels in use: twice the farthest channel edge
 * from the centre (auto mode only; the whole capture when none is open). */
static void radio_set_span(struct radio* r, double half_span)
{
    if (r->file || !r->dev || r->bw_opt >= 0)
        return;
    const double bw = radio_bw_clamp(r, half_span > 0 ? 2 * 1.15 * half_span : r->rate);
    if (fabs(bw - r->bw_set) < 0.05e6)
        return;
    r->bw_set = r->drv->set_bandwidth(r->dev, bw);
    fprintf(stderr, "%s: analog bandwidth %.2f MHz\n", r->info.name, r->bw_set / 1e6);
}

/* Start feeding mt (or the sink) from the configured source. */
static int radio_start(struct radio* r, drx_mt* mt)
{
    if (!r->file && !r->dev)
        return -1;
    r->mt = mt;
    r->samples = r->overflows = r->filled = 0;
    r->eof = 0;
    r->running = 1;
    pthread_create(&r->thread, NULL, r->file ? file_thread : sdr_thread, r);
    r->started = 1;
    return 0;
}

static void radio_stop(struct radio* r)
{
    if (!r->started)
        return;
    r->running = 0;
    pthread_join(r->thread, NULL);
    r->started = 0;
}

static void radio_close(struct radio* r)
{
    radio_stop(r);
    if (r->dev) {
        r->drv->close(r->dev);
        r->dev = NULL;
    }
}

/* ---- standalone output ---------------------------------------------------- */

struct output {
    FILE* file;
    int sock;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    int per_isi_port;
    uint8_t dgram[256][7 * 188]; /* per-ISI UDP datagram being filled */
    int fill[256];
};

static int output_open(struct output* o, const char* dest, int all_isi)
{
    memset(o, 0, sizeof(*o));
    o->sock = -1;
    if (!strncmp(dest, "udp://", 6)) {
        char host[256];
        snprintf(host, sizeof(host), "%s", dest + 6);
        char* colon = strrchr(host, ':');
        if (!colon)
            return -1;
        *colon = 0;
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *ai;
        if (getaddrinfo(host, colon + 1, &hints, &ai))
            return -1;
        o->sock = socket(ai->ai_family, SOCK_DGRAM, 0);
        memcpy(&o->addr, ai->ai_addr, ai->ai_addrlen);
        o->addrlen = ai->ai_addrlen;
        freeaddrinfo(ai);
        o->per_isi_port = all_isi;
        return o->sock < 0 ? -1 : 0;
    }
    o->file = strcmp(dest, "-") ? fopen(dest, "wb") : stdout;
    return o->file ? 0 : -1;
}

static void on_ts_output(void* ctx, int isi, const uint8_t* ts, int len)
{
    struct output* o = ctx;
    if (o->file) {
        fwrite(ts, 1, len, o->file);
        return;
    }
    for (int p = 0; p < len; p += 188) {
        memcpy(o->dgram[isi] + o->fill[isi], ts + p, 188);
        o->fill[isi] += 188;
        if (o->fill[isi] == 7 * 188) {
            struct sockaddr_storage a = o->addr;
            if (o->per_isi_port) {
                if (a.ss_family == AF_INET)
                    ((struct sockaddr_in*)&a)->sin_port =
                        htons(ntohs(((struct sockaddr_in*)&a)->sin_port) + isi);
                else
                    ((struct sockaddr_in6*)&a)->sin6_port =
                        htons(ntohs(((struct sockaddr_in6*)&a)->sin6_port) + isi);
            }
            sendto(o->sock, o->dgram[isi], o->fill[isi], 0, (struct sockaddr*)&a, o->addrlen);
            o->fill[isi] = 0;
        }
    }
}

/* ---- statistics ----------------------------------------------------------- */

static void print_stats(drx_mt* mt, const struct radio* r, double t0, double symrate,
                        const char* tag)
{
    drx_rx_stats s;
    drx_mt_get_stats(mt, &s);
    fprintf(stderr,
            "%s[%6.1f s] %s %s %s%s%s SNR %5.2f dB freq %+8.1f kHz | frames %llu "
            "ldpc it %.1f fail %llu | bch fail %llu | ts %llu err %llu | in %.1f Msps "
            "drop %llu ovf %llu (%.0f ms) | %.1f dBFS%s gain %.0f\n",
            tag, now() - t0, s.locked ? "LOCK" : "----", s.constellation ? s.constellation : "-",
            s.rate ? s.rate : "-", s.short_frame ? " short" : "", s.pilots ? " pilots" : "",
            s.snr_db, s.freq * symrate / 1e3, (unsigned long long)s.plframes, s.ldpc_avg_iter,
            (unsigned long long)s.ldpc_failed_batches, (unsigned long long)s.bch_failed,
            (unsigned long long)s.ts_packets, (unsigned long long)s.ts_crc_errors,
            r->samples / (now() - t0) / 1e6, (unsigned long long)drx_mt_dropped(mt),
            (unsigned long long)r->overflows, r->rate > 0 ? 1e3 * r->filled / r->rate : 0.0,
            r->file ? 0.0f : r->level_dbfs, r->clip_ppm > 0 ? " CLIP" : "", r->file ? 0.0 : r->gain);
}

/* ---- LNB controller ------------------------------------------------------- */

struct lnb {
    int fd; /* -1: no controller */
    int is_udp;
    /* last state set, for adapters sharing the LNB (-1: unknown) */
    int voltage, tone;
    uint8_t diseqc[6];
    int diseqc_len;
};

static int lnb_open(struct lnb* l, const char* spec)
{
    l->fd = -1;
    l->is_udp = 0;
    l->voltage = l->tone = l->diseqc_len = -1;
    if (!spec || !strcmp(spec, "none"))
        return 0;
    if (!strncmp(spec, "serial:", 7)) {
        l->fd = open(spec + 7, O_RDWR | O_NOCTTY);
        if (l->fd < 0) {
            perror(spec + 7);
            return -1;
        }
        struct termios t;
        tcgetattr(l->fd, &t);
        cfmakeraw(&t);
        cfsetspeed(&t, B115200);
        tcsetattr(l->fd, TCSANOW, &t);
        return 0;
    }
    if (!strncmp(spec, "udp:", 4)) {
        char host[256];
        snprintf(host, sizeof(host), "%s", spec + 4);
        char* colon = strrchr(host, ':');
        if (!colon)
            return -1;
        *colon = 0;
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *ai;
        if (getaddrinfo(host, colon + 1, &hints, &ai))
            return -1;
        l->fd = socket(ai->ai_family, SOCK_DGRAM, 0);
        if (l->fd >= 0 && connect(l->fd, ai->ai_addr, ai->ai_addrlen)) {
            close(l->fd);
            l->fd = -1;
        }
        freeaddrinfo(ai);
        l->is_udp = 1;
        return l->fd < 0 ? -1 : 0;
    }
    fprintf(stderr, "unknown LNB controller \"%s\"\n", spec);
    return -1;
}

/* Sends one command line, waits for "OK" or "ERR ...". Returns 0 or -EIO. */
static int lnb_command(struct lnb* l, const char* cmd)
{
    fprintf(stderr, "LNB: %s\n", cmd);
    if (l->fd < 0)
        return 0;
    char line[128];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(l->fd, line, n) != n)
        return -EIO;
    /* one reply line, 1 s timeout */
    size_t got = 0;
    const double deadline = now() + 1.0;
    while (got < sizeof(line) - 1) {
        struct pollfd p = { .fd = l->fd, .events = POLLIN };
        int ms = (int)((deadline - now()) * 1e3);
        if (ms <= 0 || poll(&p, 1, ms) <= 0)
            break;
        ssize_t r = read(l->fd, line + got, sizeof(line) - 1 - got);
        if (r <= 0)
            break;
        got += r;
        if (l->is_udp || memchr(line, '\n', got))
            break;
    }
    line[got] = 0;
    if (!strncmp(line, "OK", 2))
        return 0;
    fprintf(stderr, "LNB: controller answered \"%s\"\n", got ? strtok(line, "\r\n") : "nothing");
    return -EIO;
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

/* ---- receiver settings ---------------------------------------------------- */

/* Low roll-offs have slowly decaying RRC tails: keep the truncation loss
 * small without paying for long filters where they are not needed. */
static int rrc_delay_for(float rolloff)
{
    if (rolloff >= 0.2f)
        return 10;
    int d = (int)(2.0f / rolloff + 0.5f);
    return d < 10 ? 10 : d;
}

struct hub;

struct bridge {
    int index; /* /dev/dvbrxN */
    int ctl;
    struct radio* radio;
    struct lnb* lnb;
    struct hub* hub; /* several adapters on one capture, or NULL */
    drx_mt* mt;
    int threads, rrc_delay;
    double max_rate;
    double symrate;
    double ch_freq, ch_occ; /* hub: channel centre and occupied bandwidth, Hz */
    int ch_id;              /* hub: channelizer output */
    uint64_t last_frames, last_packets;
    double t_frames, t_packets; /* when they last went up */
    double t0, last_status, last_print;
    /* control socket (dvbrx-web) */
    struct dvbrx_tune tune; /* the last tuning */
    int mode;               /* who uses the receiver: 0 the application, 1 a probe, 2 the monitor */
    uint64_t gen;           /* bumped at every tuning or stop */
    psi* psi;               /* probe: service information collected */
    double rate_t;          /* TS rate: last sample */
    uint64_t rate_pk;
    double ts_mbps;
};

/* ---- several adapters on one wideband capture ----------------------------- */

#define MAX_BRIDGES 8
#define HUB_USABLE 0.45 /* usable half-band, fraction of the capture rate */

struct hub {
    struct radio* radio;
    double fs;     /* capture rate */
    double lo;     /* capture centre, Hz */
    int fixed_lo;  /* -f given: never retuned */
    int streaming; /* the radio thread runs */
    drx_chan* chan;
    struct bridge* b[MAX_BRIDGES];
    int n;
};

/* Radio thread: the channelizer splits the capture, each channel goes to
 * its adapter's receiver. */
static void hub_push(void* ctx, const float* iq, int n)
{
    struct hub* h = ctx;
    drx_chan_process(h->chan, iq, n);
}

static void hub_deliver(void* ctx, const float* iq, int n)
{
    struct bridge* b = ctx;
    drx_mt* mt = __atomic_load_n(&b->mt, __ATOMIC_ACQUIRE);
    if (mt)
        drx_mt_push(mt, iq, n, 1);
}

static int hub_active(const struct hub* h, const struct bridge* except)
{
    int n = 0;
    for (int i = 0; i < h->n; i++)
        n += h->b[i] != except && h->b[i]->mt;
    return n;
}

static int hub_fits(const struct hub* h, double lo, double f, double occ)
{
    return fabs(f - lo) + occ / 2 <= HUB_USABLE * h->fs;
}

/* Chooses the capture centre for a new channel of adapter b, keeping the
 * channels of the other adapters inside the capture; retunes if needed.
 * Returns 0, or -1 when the channel does not fit. */
/* Analog filter around the channels open on the capture (b: being placed). */
static void hub_filter(struct hub* h, const struct bridge* b)
{
    double half = 0;
    for (int i = 0; i < h->n; i++) {
        const struct bridge* o = h->b[i];
        if (o != b && !o->mt)
            continue;
        const double e = fabs(o->ch_freq - h->lo) + o->ch_occ / 2;
        half = e > half ? e : half;
    }
    radio_set_span(h->radio, half);
}

static int hub_place(struct hub* h, struct bridge* b, double f, double occ)
{
    double lo = h->lo;
    if (occ / 2 > HUB_USABLE * h->fs) {
        fprintf(stderr, "adapter %d: %.3f MHz wide, more than the capture (%.3f Msps)\n",
                b->index, occ / 1e6, h->fs / 1e6);
        return -1;
    }
    if (h->fixed_lo) {
        if (!hub_fits(h, lo, f, occ)) {
            fprintf(stderr, "adapter %d: %.3f MHz is outside the capture (%.3f MHz +- %.3f)\n",
                    b->index, f / 1e6, lo / 1e6, HUB_USABLE * h->fs / 1e6);
            return -1;
        }
    } else if (!hub_active(h, b)) {
        lo = f;
    } else {
        double lo_edge = f - occ / 2, hi_edge = f + occ / 2;
        int fits = hub_fits(h, lo, f, occ);
        for (int i = 0; i < h->n; i++) {
            const struct bridge* o = h->b[i];
            if (o == b || !o->mt)
                continue;
            if (o->ch_freq - o->ch_occ / 2 < lo_edge)
                lo_edge = o->ch_freq - o->ch_occ / 2;
            if (o->ch_freq + o->ch_occ / 2 > hi_edge)
                hi_edge = o->ch_freq + o->ch_occ / 2;
            fits = fits && hub_fits(h, lo, o->ch_freq, o->ch_occ);
        }
        if (hi_edge - lo_edge > 2 * HUB_USABLE * h->fs) {
            fprintf(stderr,
                    "adapter %d: %.3f MHz does not fit in the capture with the other adapters' "
                    "channels (%.3f to %.3f MHz, %.3f MHz usable)\n",
                    b->index, f / 1e6, lo_edge / 1e6, hi_edge / 1e6, 2 * HUB_USABLE * h->fs / 1e6);
            return -1;
        }
        if (!fits)
            lo = (lo_edge + hi_edge) / 2;
    }
    b->ch_freq = f;
    b->ch_occ = occ;
    if (lo != h->lo || !h->streaming) {
        if (h->streaming)
            radio_retune(h->radio, lo);
        for (int i = 0; i < h->n; i++) {
            struct bridge* o = h->b[i];
            if (o != b && o->mt)
                drx_chan_set_offset(h->chan, o->ch_id, o->ch_freq - lo);
        }
        if (lo != h->lo)
            fprintf(stderr, "capture centre %.3f MHz (%d other channel%s)\n", lo / 1e6,
                    hub_active(h, b), hub_active(h, b) > 1 ? "s" : "");
        h->lo = lo;
    }
    hub_filter(h, b);
    return 0;
}

static void hub_attach(struct hub* h, struct bridge* b, drx_mt* mt)
{
    __atomic_store_n(&b->mt, mt, __ATOMIC_RELEASE);
    if (!h->streaming) {
        radio_retune(h->radio, h->lo);
        h->radio->sink = hub_push;
        h->radio->sink_ctx = h;
        radio_start(h->radio, NULL);
        h->streaming = 1;
    }
}

static void hub_detach(struct hub* h, struct bridge* b)
{
    /* once removed, the channelizer no longer calls hub_deliver for b */
    drx_chan_remove(h->chan, b->ch_id);
    b->ch_id = -1;
    drx_mt* mt = b->mt;
    b->mt = NULL;
    if (h->streaming && !hub_active(h, NULL)) {
        radio_stop(h->radio);
        h->streaming = 0;
    } else {
        hub_filter(h, NULL);
    }
    drx_mt_free(mt);
}

/* ---- DVB bridge ----------------------------------------------------------- */

static void on_ts_bridge(void* ctx, int isi, const uint8_t* ts, int len)
{
    (void)isi;
    struct bridge* b = ctx;
    if (b->mode) { /* a probe or the monitor: not for the application */
        if (b->psi)
            psi_feed(b->psi, ts, len);
        return;
    }
    if (write(b->ctl, ts, len) != len)
        perror("write to /dev/dvbrx");
}

static float rolloff_of(const struct dvbrx_tune* t)
{
    if (t->delivery_system == SYS_DVBS)
        return 0.35f; /* fixed by EN 300 421; the cache may hold a DVB-S2 value */
    if (t->delivery_system == SYS_DVBC_ANNEX_A)
        return 0.15f; /* EN 300 429 */
    switch (t->rolloff) {
    case ROLLOFF_35: return 0.35f;
    case ROLLOFF_25: return 0.25f;
    case ROLLOFF_20: return 0.20f;
    case ROLLOFF_15: return 0.15f;
    case ROLLOFF_10: return 0.10f;
    case ROLLOFF_5: return 0.05f;
    default: return t->delivery_system == SYS_DVBS ? 0.35f : 0.20f;
    }
}

static int is_ofdm(const struct dvbrx_tune* t)
{
    return t->delivery_system == SYS_DVBT || t->delivery_system == SYS_DVBT2;
}

/* What a channel needs: the sample rate it is best received at, the least
 * it can do with, and its occupied bandwidth (Hz). */
struct chan_need {
    double want, min, occ;
};

static struct chan_need need_of(const struct dvbrx_tune* t)
{
    struct chan_need n;
    if (is_ofdm(t)) {
        const double bw = t->bandwidth_hz ? t->bandwidth_hz : 8e6, elem = bw * 8 / 7;
        n.want = elem * 1.12;
        n.min = elem;
        n.occ = 0.98 * bw; /* DVB-T2 extended carriers: 7.77 MHz in 8 MHz */
    } else {
        n.want = 2.0 * t->symbol_rate;
        n.min = 1.5 * t->symbol_rate;
        n.occ = t->symbol_rate * (1 + rolloff_of(t));
    }
    return n;
}

/* enum fe_modulation -> enum drx_qam */
static int qam_of(uint32_t modulation)
{
    switch (modulation) {
    case QAM_16: return DRX_QAM_16;
    case QAM_32: return DRX_QAM_32;
    case QAM_64: return DRX_QAM_64;
    case QAM_128: return DRX_QAM_128;
    case QAM_256: return DRX_QAM_256;
    default: return DRX_QAM_AUTO;
    }
}

/* Receiver settings for a tuning request, the channel coming at rate. */
static void rx_cfg_for(const struct bridge* b, const struct dvbrx_tune* t, double rate,
                       drx_mt_cfg* cfg)
{
    if (is_ofdm(t)) {
        drx_mt_cfg_default(cfg, 2, 0.2f);
        cfg->dvbt = t->delivery_system == SYS_DVBT;
        cfg->dvbt2 = t->delivery_system == SYS_DVBT2;
        cfg->sample_rate = rate;
        cfg->bandwidth = t->bandwidth_hz ? t->bandwidth_hz : 8e6;
        cfg->rx.isi = t->stream_id >= 0 ? t->stream_id : DRX_ISI_FIRST; /* T2: the PLP */
        return;
    }
    const float rolloff = rolloff_of(t);
    drx_mt_cfg_default(cfg, (float)(rate / t->symbol_rate), rolloff);
    cfg->ldpc_threads = b->threads;
    cfg->fe.rrc_delay = b->rrc_delay > 0 ? b->rrc_delay : rrc_delay_for(rolloff);
    cfg->rx.gold_code = (int)t->scrambling_index;
    /* Applications commonly send 0 when no stream was asked for: like no
     * filter, take the first stream (a single-stream signal is ISI 0). */
    cfg->rx.isi = t->stream_id > 0 ? t->stream_id : DRX_ISI_FIRST;
    if (t->delivery_system == SYS_DVBS) {
        cfg->dvbs = 1;
        switch (t->fec_inner) {
        case FEC_1_2: cfg->dvbs_rate = DRX_DVBS_1_2; break;
        case FEC_2_3: cfg->dvbs_rate = DRX_DVBS_2_3; break;
        case FEC_3_4: cfg->dvbs_rate = DRX_DVBS_3_4; break;
        case FEC_5_6: cfg->dvbs_rate = DRX_DVBS_5_6; break;
        case FEC_7_8: cfg->dvbs_rate = DRX_DVBS_7_8; break;
        default: cfg->dvbs_rate = DRX_DVBS_AUTO; break;
        }
    }
    if (t->delivery_system == SYS_DVBC_ANNEX_A) {
        cfg->dvbc = 1;
        cfg->dvbc_qam = qam_of(t->modulation);
        cfg->dvbc_inversion = t->inversion <= 2 ? (int)t->inversion : 2;
    }
}

static void bridge_stop_rx(struct bridge* b)
{
    b->gen++;
    if (b->hub) {
        hub_detach(b->hub, b);
        return;
    }
    radio_stop(b->radio);
    if (b->mt) {
        drx_mt_free(b->mt);
        b->mt = NULL;
    }
}

/* Single-channel mode: the SDR rate follows the channel. Sample rate for
 * an OFDM channel: a little above the elementary rate, as the device
 * allows; for single-carrier ones about 2 samples per symbol. */
static double single_rate(struct bridge* b, const struct dvbrx_tune* t, const struct chan_need* n)
{
    const double freq = t->frequency_khz * 1e3;
    double want = n->want > b->max_rate ? b->max_rate : n->want;
    if (want < n->min)
        return -1;
    b->radio->bw_need = n->occ;
    double rate = radio_configure(b->radio, freq, want);
    if (is_ofdm(t) && rate > 0 && rate < n->min) /* rounded down below what is needed */
        rate = radio_configure(b->radio, freq, n->min * 1.3);
    return rate >= n->min ? rate : -1;
}

static void log_tune(const struct bridge* b, const struct dvbrx_tune* t)
{
    char tag[16] = "";
    if (b->hub)
        snprintf(tag, sizeof(tag), "adapter %d: ", b->index);
    switch (t->delivery_system) {
    case SYS_DVBT2:
        fprintf(stderr, "%stune: DVB-T2, %u kHz, bandwidth %.1f MHz, PLP %d\n", tag,
                t->frequency_khz, (t->bandwidth_hz ? t->bandwidth_hz : 8e6) / 1e6, t->stream_id);
        break;
    case SYS_DVBT:
        fprintf(stderr, "%stune: DVB-T, %u kHz, bandwidth %.0f MHz\n", tag, t->frequency_khz,
                (t->bandwidth_hz ? t->bandwidth_hz : 8e6) / 1e6);
        break;
    case SYS_DVBS:
        fprintf(stderr, "%stune: DVB-S, %u kHz, %u sym/s\n", tag, t->frequency_khz, t->symbol_rate);
        break;
    case SYS_DVBC_ANNEX_A:
        fprintf(stderr, "%stune: DVB-C, %u kHz, %u sym/s, QAM %d\n", tag, t->frequency_khz,
                t->symbol_rate, qam_of(t->modulation));
        break;
    default:
        fprintf(stderr, "%stune: DVB-S2, %u kHz, %u sym/s, roll-off %.2f, ISI %d, gold %u\n", tag,
                t->frequency_khz, t->symbol_rate, rolloff_of(t), t->stream_id, t->scrambling_index);
    }
}

static void bridge_tune(struct bridge* b, const struct dvbrx_tune* t)
{
    bridge_stop_rx(b);
    if (t->delivery_system != SYS_DVBS && t->delivery_system != SYS_DVBS2 &&
        t->delivery_system != SYS_DVBC_ANNEX_A && t->delivery_system != SYS_DVBT &&
        t->delivery_system != SYS_DVBT2) {
        fprintf(stderr, "tune: delivery system %u not supported yet\n", t->delivery_system);
        return;
    }
    log_tune(b, t);
    b->tune = *t;
    if (!is_ofdm(t) && t->symbol_rate == 0) {
        fprintf(stderr, "tune: symbol rate out of range\n");
        return;
    }
    const struct chan_need need = need_of(t);
    const double freq = t->frequency_khz * 1e3;
    double rate;
    drx_mt_cfg cfg;
    if (b->hub) {
        struct hub* h = b->hub;
        if (h->fs < need.min) {
            fprintf(stderr, "adapter %d: needs %.3f Msps, more than the capture (%.3f Msps)\n",
                    b->index, need.min / 1e6, h->fs / 1e6);
            return;
        }
        if (hub_place(h, b, freq, need.occ))
            return;
        /* the channel's samples are dropped until its receiver is there */
        b->ch_id = drx_chan_add(h->chan, freq - h->lo, need.occ, need.want, &rate, hub_deliver, b);
        if (b->ch_id < 0 || rate < need.min) {
            fprintf(stderr, "adapter %d: channel does not fit in the capture\n", b->index);
            drx_chan_remove(h->chan, b->ch_id);
            b->ch_id = -1;
            return;
        }
        rx_cfg_for(b, t, rate, &cfg);
        fprintf(stderr, "adapter %d: channel at %+.3f MHz in the capture, %.3f Msps\n", b->index,
                (freq - h->lo) / 1e6, rate / 1e6);
    } else {
        rate = single_rate(b, t, &need);
        if (rate <= 0) {
            fprintf(stderr, "tune: no usable sample rate (%.3f Msps needed)\n", need.min / 1e6);
            return;
        }
        rx_cfg_for(b, t, rate, &cfg);
    }
    drx_mt* mt = drx_mt_new(&cfg, on_ts_bridge, b);
    if (!mt) {
        fprintf(stderr, "tune: receiver setup failed\n");
        if (b->hub) {
            drx_chan_remove(b->hub->chan, b->ch_id);
            b->ch_id = -1;
        }
        return;
    }
    /* statistics: OFDM frequency in cycles per input sample, else per symbol */
    b->symrate = is_ofdm(t) ? rate : t->symbol_rate;
    b->last_frames = b->last_packets = 0;
    b->t_frames = b->t_packets = 0;
    b->t0 = now();
    if (b->hub) {
        hub_attach(b->hub, b, mt);
    } else {
        b->mt = mt;
        if (radio_start(b->radio, b->mt))
            bridge_stop_rx(b);
    }
}

static int lnb_event(struct bridge* b, const struct dvbrx_event* ev)
{
    struct lnb* l = b->lnb;
    /* Adapters sharing the LNB: a command that would change what another
     * adapter receives is refused while that adapter is in use; repeating
     * the current state is accepted (and not sent again). */
    const int shared = b->hub && hub_active(b->hub, b);
    char cmd[64];
    switch (ev->type) {
    case DVBRX_EV_VOLTAGE:
        if (shared && (ev->voltage == (uint32_t)l->voltage || ev->voltage == SEC_VOLTAGE_OFF))
            return 0;
        if (shared)
            return -EBUSY;
        l->voltage = (int)ev->voltage;
        snprintf(cmd, sizeof(cmd), "VOLT %s",
                 ev->voltage == SEC_VOLTAGE_13   ? "13"
                 : ev->voltage == SEC_VOLTAGE_18 ? "18"
                                                 : "OFF");
        break;
    case DVBRX_EV_TONE:
        if (shared)
            return ev->tone == (uint32_t)l->tone ? 0 : -EBUSY;
        l->tone = (int)ev->tone;
        snprintf(cmd, sizeof(cmd), "TONE %s", ev->tone == SEC_TONE_ON ? "ON" : "OFF");
        break;
    case DVBRX_EV_BURST:
        if (shared)
            return -EBUSY;
        snprintf(cmd, sizeof(cmd), "BURST %s", ev->burst == SEC_MINI_A ? "A" : "B");
        break;
    case DVBRX_EV_DISEQC: {
        const int len = ev->diseqc.len < 6 ? ev->diseqc.len : 6;
        if (shared)
            return len == l->diseqc_len && !memcmp(ev->diseqc.msg, l->diseqc, len) ? 0 : -EBUSY;
        l->diseqc_len = len;
        memcpy(l->diseqc, ev->diseqc.msg, len);
        int n = snprintf(cmd, sizeof(cmd), "DISEQC");
        for (int i = 0; i < len; i++)
            n += snprintf(cmd + n, sizeof(cmd) - n, " %02x", ev->diseqc.msg[i]);
        break;
    }
    default:
        return -EINVAL;
    }
    return lnb_command(l, cmd);
}

/* ---- control socket (dvbrx-web, see doc/dvbrx-web-api.md) ---------------- */

/* One lock for what changes a bridge's receiver: the event loop (the
 * application's tunings) takes it per event, the control threads around
 * their own tunings; status reads take it briefly. Never held while
 * waiting (a probe sleeps without it). */
static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static struct bridge* ctl_b;
static int ctl_n;
static double ctl_input_offset = 0; /* -P: dBm at 0 dBFS and 0 dB gain (estimate) */
static double ctl_last_status;        /* for the monitor watchdog */

struct sbuf {
    char* s;
    size_t n, cap;
};

static void sb_printf(struct sbuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(struct sbuf* b, const char* fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        const int k = vsnprintf(b->s ? b->s + b->n : NULL, b->s ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0)
            return;
        if (b->s && b->n + (size_t)k < b->cap) {
            b->n += (size_t)k;
            return;
        }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + (size_t)k + 1)
            cap *= 2;
        char* t = realloc(b->s, cap);
        if (!t)
            return;
        b->s = t;
        b->cap = cap;
    }
}

/* number as JSON, null when not finite */
static void sb_num(struct sbuf* b, const char* key, double v, int prec)
{
    if (isfinite(v))
        sb_printf(b, "\"%s\": %.*f", key, prec, v);
    else
        sb_printf(b, "\"%s\": null", key);
}

/* Minimal JSON reading for flat requests: the value after "key":. */
static const char* json_val(const char* req, const char* key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(req, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static double json_num(const char* req, const char* key, double dflt)
{
    const char* p = json_val(req, key);
    if (!p || !(*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')))
        return dflt;
    return atof(p);
}

static int json_bool(const char* req, const char* key, int dflt)
{
    const char* p = json_val(req, key);
    if (!p)
        return dflt;
    return !strncmp(p, "true", 4) ? 1 : !strncmp(p, "false", 5) ? 0 : dflt;
}

static int json_str(const char* req, const char* key, char* out, size_t n)
{
    const char* p = json_val(req, key);
    out[0] = 0;
    if (!p || *p != '"')
        return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n)
        out[i++] = *p++;
    out[i] = 0;
    return 1;
}

static const char* delsys_name(uint32_t d)
{
    switch (d) {
    case SYS_DVBS: return "DVB-S";
    case SYS_DVBS2: return "DVB-S2";
    case SYS_DVBC_ANNEX_A: return "DVB-C";
    case SYS_DVBT: return "DVB-T";
    case SYS_DVBT2: return "DVB-T2";
    }
    return "?";
}

/* the application uses the receiver (a probe would take it away) */
static int app_busy(const struct bridge* b)
{
    for (int i = 0; i < ctl_n; i++)
        if (ctl_b[i].mt && ctl_b[i].mode == 0)
            return 1;
    (void)b;
    return 0;
}

static void status_json(struct sbuf* o)
{
    const struct radio* r = ctl_b[0].radio;
    const int running = r->started && r->running;
    sb_printf(o, "{\"ok\": true, \"radio\": {\"source\": \"%s\", ", r->file ? "file" : r->drv ? r->drv->prefix : "none");
    sb_printf(o, "\"rate_max\": %.0f, \"multi\": %s, ", r->file ? 61.44e6 : r->dev ? r->info.rate_max : r->drv ? r->drv->rate_max : 0,
              r->file || (r->drv && r->drv->multi) ? "true" : "false");
    sb_num(o, "freq", r->freq, 0);
    sb_printf(o, ", ");
    sb_num(o, "rate", r->rate, 0);
    sb_printf(o, ", ");
    sb_num(o, "bandwidth", r->bw_set > 0 ? r->bw_set : NAN, 0);
    sb_printf(o, ", ");
    sb_num(o, "gain", r->file ? NAN : r->gain, 1);
    static const char* const agc[] = { "level", "search", "best" };
    sb_printf(o, ", \"agc\": \"%s\", ", !r->gain_auto ? "manual" : agc[r->agc_state % 3]);
    sb_num(o, "level_dbfs", running ? r->level_dbfs : NAN, 1);
    sb_printf(o, ", ");
    sb_num(o, "input_dbm", running && !r->file ? r->level_dbfs - r->gain + ctl_input_offset : NAN, 1);
    sb_printf(o, ", \"overflows\": %llu, \"filled\": %llu, \"running\": %s", (unsigned long long)r->overflows,
              (unsigned long long)r->filled, running ? "true" : "false");
    const struct lnb* l = ctl_b[0].lnb && ctl_b[0].lnb->fd >= 0 ? ctl_b[0].lnb : NULL;
    sb_printf(o, ", \"lnb\": {\"present\": %s, \"volt\": \"%s\", \"tone\": %s}}, \"channels\": [",
              l ? "true" : "false",
              !l ? "off" : l->voltage == SEC_VOLTAGE_13 ? "13" : l->voltage == SEC_VOLTAGE_18 ? "18" : "off",
              l && l->tone == SEC_TONE_ON ? "true" : "false");
    for (int i = 0; i < ctl_n; i++) {
        const struct bridge* b = &ctl_b[i];
        static const char* const modes[] = { "tuned", "probe", "monitor" };
        sb_printf(o, "%s{\"adapter\": %d, \"state\": \"%s\"", i ? ", " : "", b->index,
                  b->mt ? modes[b->mode % 3] : b->mode == 2 ? "monitor" : "idle");
        if (b->mt || b->mode == 2) {
            const struct dvbrx_tune* t = &b->tune;
            sb_printf(o, ", \"delsys\": \"%s\", \"freq\": %.0f, \"symbol_rate\": %u, \"bandwidth\": %u",
                      delsys_name(t->delivery_system), t->frequency_khz * 1e3,
                      is_ofdm(t) ? 0 : t->symbol_rate, is_ofdm(t) ? (t->bandwidth_hz ? t->bandwidth_hz : 8000000) : 0);
            sb_printf(o, ", \"rolloff\": %.2f, \"plp\": %d", is_ofdm(t) || t->delivery_system == SYS_DVBC_ANNEX_A ? 0.0 : rolloff_of(t),
                      t->stream_id);
        }
        if (b->mt) {
            drx_rx_stats s;
            drx_mt_get_stats(b->mt, &s);
            char rate[16] = "";
            if (s.rate) {
                snprintf(rate, sizeof(rate), "%s", s.rate);
                for (char* c = rate; *c; c++)
                    if (*c == '_')
                        *c = '/';
            }
            sb_printf(o, ", \"locked\": %s, ", s.locked ? "true" : "false");
            sb_num(o, "snr_db", s.snr_db > 0 ? s.snr_db : NAN, 2);
            sb_printf(o, ", ");
            sb_num(o, "freq_offset_hz", s.freq * b->symrate, 0);
            sb_printf(o, ", \"modulation\": %s%s%s, \"code_rate\": %s%s%s", s.constellation ? "\"" : "",
                      s.constellation ? s.constellation : "null", s.constellation ? "\"" : "", rate[0] ? "\"" : "",
                      rate[0] ? rate : "null", rate[0] ? "\"" : "");
            if (b->tune.delivery_system == SYS_DVBS2)
                sb_printf(o, ", \"pilots\": %s", s.pilots ? "true" : "false");
            sb_printf(o, ", \"ts_packets\": %llu, \"ts_errors\": %llu, ", (unsigned long long)s.ts_packets,
                      (unsigned long long)s.ts_crc_errors);
            sb_num(o, "ts_mbps", b->ts_mbps, 2);
            sb_printf(o, ", \"fec_blocks\": %llu, \"fec_errors\": %llu, ", (unsigned long long)s.plframes,
                      (unsigned long long)s.bch_failed);
            sb_num(o, "uptime", now() - b->t0, 1);
        }
        if (b->mt || b->mode == 2) { /* the radio's level, for this channel */
            sb_printf(o, ", ");
            sb_num(o, "gain", r->file ? NAN : r->gain, 1);
            sb_printf(o, ", ");
            sb_num(o, "level_dbfs", running ? r->level_dbfs : NAN, 1);
            sb_printf(o, ", ");
            sb_num(o, "input_dbm", running && !r->file ? r->level_dbfs - r->gain + ctl_input_offset : NAN, 1);
        }
        sb_printf(o, "}");
    }
    sb_printf(o, "]}");
}

/* Releases a probe or the monitor (lock held). */
static void ctl_release(struct bridge* b)
{
    if (b->mode == 2 && !b->mt) { /* level-only monitor: the radio alone */
        radio_stop(b->radio);
        b->radio->sink = NULL;
    } else {
        bridge_stop_rx(b);
    }
    b->mode = 0;
    psi_free(b->psi);
    b->psi = NULL;
}

/* A tuning request from JSON (the frequency is what the B210 sees). */
static int tune_of(const char* req, struct dvbrx_tune* t, char* err, size_t errn)
{
    char sys[16];
    memset(t, 0, sizeof(*t));
    json_str(req, "delsys", sys, sizeof(sys));
    t->delivery_system = !strcmp(sys, "DVB-S")    ? SYS_DVBS
                         : !strcmp(sys, "DVB-S2") ? SYS_DVBS2
                         : !strcmp(sys, "DVB-C")  ? SYS_DVBC_ANNEX_A
                         : !strcmp(sys, "DVB-T")  ? SYS_DVBT
                         : !strcmp(sys, "DVB-T2") ? SYS_DVBT2
                                                  : 0;
    if (!t->delivery_system) {
        snprintf(err, errn, "unknown delsys \"%s\"", sys);
        return -1;
    }
    t->frequency_khz = (uint32_t)llround(json_num(req, "freq", 0) / 1e3);
    t->symbol_rate = (uint32_t)llround(json_num(req, "symbol_rate", 0));
    t->bandwidth_hz = (uint32_t)llround(json_num(req, "bandwidth", 0));
    if (!t->bandwidth_hz)
        t->bandwidth_hz = 8000000;
    const double ro = json_num(req, "rolloff", 0.2);
    t->rolloff = ro < 0.075 ? ROLLOFF_5 : ro < 0.125 ? ROLLOFF_10 : ro < 0.175 ? ROLLOFF_15
                 : ro < 0.225 ? ROLLOFF_20 : ro < 0.3 ? ROLLOFF_25 : ROLLOFF_35;
    t->stream_id = (int32_t)json_num(req, "plp", -1);
    t->inversion = INVERSION_AUTO;
    t->modulation = QAM_AUTO;
    char mod[16];
    if (json_str(req, "modulation", mod, sizeof(mod)) && t->delivery_system == SYS_DVBC_ANNEX_A) {
        const int q = atoi(mod + (strncmp(mod, "QAM", 3) ? 0 : 3));
        t->modulation = q == 16 ? QAM_16 : q == 32 ? QAM_32 : q == 64 ? QAM_64 : q == 128 ? QAM_128
                        : q == 256 ? QAM_256 : QAM_AUTO;
    }
    if (!t->frequency_khz) {
        snprintf(err, errn, "missing freq");
        return -1;
    }
    return 0;
}

static void discard_sink(void* ctx, const float* iq, int n)
{
    (void)ctx;
    (void)iq;
    (void)n;
}

static int radio_capture_at(struct radio* r, double freq, double rate, double gain, double bw, int n,
                            float* out, double* level);

/* n samples at freq/rate on an idle radio (lock held); gain < 0: auto.
 * *gain_used: the gain of the capture; the radio's gain is left as it was
 * (the next tuning starts from it, not from a capture's). */
static int radio_capture(struct radio* r, double freq, double rate, double gain, double bw, int n,
                         float* out, double* level, double* gain_used)
{
    const double bwo = r->bw_opt, bwn = r->bw_need, g0 = r->gain;
    const int rc = radio_capture_at(r, freq, rate, gain, bw, n, out, level);
    *gain_used = r->gain;
    r->gain = g0;
    r->bw_opt = bwo;
    r->bw_need = bwn;
    return rc;
}

static int radio_capture_at(struct radio* r, double freq, double rate, double gain, double bw, int n,
                            float* out, double* level)
{
    const int auto_g = gain < 0;
    r->gain = auto_g ? 30 : gain;
    r->bw_opt = bw > 0 ? bw : -1;
    r->bw_need = 0; /* bw 0: the whole capture */
    const double got_rate = radio_configure(r, freq, rate);
    if (got_rate <= 0 || r->file)
        return -1;
    const struct sdr_driver* d = r->drv;
    for (int pass = 0; pass < (auto_g ? 3 : 1); pass++) {
        const int skip = (int)(0.02 * r->rate) + 4096;
        const int want = pass < (auto_g ? 2 : 0) ? 16384 : n;
        d->start(r->dev);
        int got = -skip;
        float tmp[2 * 8192];
        double p = 0;
        while (got < want) {
            double t;
            enum sdr_status st;
            float* ptr = got < 0 ? tmp : out + 2 * got;
            /* the samples skipped end exactly where the capture starts */
            const int room = got < 0 ? (-got < 8192 ? -got : 8192) : want - got;
            const int k = d->read(r->dev, ptr, room, 1.0, &t, &st);
            if (k <= 0 && st != SDR_OVERFLOW)
                break;
            if (got >= 0)
                for (int i = 0; i < 2 * k; i++)
                    p += (double)out[2 * got + i] * out[2 * got + i];
            got += k;
        }
        d->stop(r->dev);
        /* drain what was in flight */
        for (int k = 0; k < 50; k++) {
            double t;
            enum sdr_status st;
            if (d->read(r->dev, tmp, 8192, 0.01, &t, &st) <= 0)
                break;
        }
        if (got < want)
            return -1;
        const double dbfs = 10 * log10(p / want + 1e-20);
        *level = dbfs;
        if (pass < (auto_g ? 2 : 0)) {
            const double g = r->gain + fmax(-20, fmin(20, -24 - dbfs));
            r->gain = d->set_gain(r->dev, fmax(r->info.gain_min, fmin(r->gain_max, round(g))));
        }
    }
    return 0;
}

static void b64(struct sbuf* o, const uint8_t* d, size_t n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char q[5] = { 0 };
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        q[0] = T[v >> 18];
        q[1] = T[(v >> 12) & 63];
        q[2] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        q[3] = i + 2 < n ? T[v & 63] : '=';
        sb_printf(o, "%s", q);
    }
}

static void ctl_request(const char* req, struct sbuf* o)
{
    char cmd[32], err[128] = "";
    json_str(req, "cmd", cmd, sizeof(cmd));
    const int ai = (int)json_num(req, "adapter", 0);
    struct bridge* b = &ctl_b[ai >= 0 && ai < ctl_n ? ai : 0];
    const int force = json_bool(req, "force", 0);
    if (!strcmp(cmd, "status")) {
        pthread_mutex_lock(&ctl_lock);
        ctl_last_status = now();
        status_json(o);
        pthread_mutex_unlock(&ctl_lock);
        return;
    }
    if (!strcmp(cmd, "constellation")) {
        int max = (int)json_num(req, "points", 2048);
        max = max < 1 ? 1 : max > 4096 ? 4096 : max;
        float* iq = malloc(sizeof(float) * 2 * max);
        pthread_mutex_lock(&ctl_lock);
        const int n = b->mt && iq ? drx_mt_constellation(b->mt, iq, max) : 0;
        drx_rx_stats s = { 0 };
        if (b->mt)
            drx_mt_get_stats(b->mt, &s);
        pthread_mutex_unlock(&ctl_lock);
        sb_printf(o, "{\"ok\": true, \"modulation\": %s%s%s, ", s.constellation ? "\"" : "",
                  s.constellation ? s.constellation : "null", s.constellation ? "\"" : "");
        sb_num(o, "snr_db", s.snr_db > 0 ? s.snr_db : NAN, 2);
        sb_printf(o, ", \"points\": [");
        for (int i = 0; i < n; i++)
            sb_printf(o, "%s[%.3f,%.3f]", i ? "," : "", iq[2 * i], iq[2 * i + 1]);
        sb_printf(o, "]}");
        free(iq);
        return;
    }
    if (!strcmp(cmd, "lnb")) {
        pthread_mutex_lock(&ctl_lock);
        struct lnb* l = b->lnb && b->lnb->fd >= 0 ? b->lnb : NULL;
        char v[8];
        int rc = 0;
        if (!l) {
            snprintf(err, sizeof(err), "no LNB controller");
        } else if (app_busy(b) && !force) { /* would cut what the application receives */
            snprintf(err, sizeof(err), "busy");
        } else {
            if (json_str(req, "volt", v, sizeof(v))) {
                l->voltage = !strcmp(v, "13") ? SEC_VOLTAGE_13 : !strcmp(v, "18") ? SEC_VOLTAGE_18 : SEC_VOLTAGE_OFF;
                char c[16];
                snprintf(c, sizeof(c), "VOLT %s", !strcmp(v, "13") ? "13" : !strcmp(v, "18") ? "18" : "OFF");
                rc |= lnb_command(l, c);
            }
            if (json_val(req, "tone")) {
                const int tone = json_bool(req, "tone", 0);
                l->tone = tone ? SEC_TONE_ON : SEC_TONE_OFF;
                rc |= lnb_command(l, tone ? "TONE ON" : "TONE OFF");
            }
            if (rc)
                snprintf(err, sizeof(err), "LNB controller error");
        }
        pthread_mutex_unlock(&ctl_lock);
        if (err[0])
            sb_printf(o, "{\"ok\": false, \"error\": \"%s\"}", err);
        else
            sb_printf(o, "{\"ok\": true}");
        return;
    }
    /* the commands below take the radio */
    if (!strcmp(cmd, "capture") || !strcmp(cmd, "probe") || !strcmp(cmd, "monitor")) {
        char action[16] = "start";
        json_str(req, "action", action, sizeof(action));
        pthread_mutex_lock(&ctl_lock);
        if (!strcmp(cmd, "monitor") && !strcmp(action, "stop")) {
            if (b->mode == 2)
                ctl_release(b);
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": true}");
            return;
        }
        if (b->hub || b->radio->file) {
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": false, \"error\": \"not available %s\"}",
                      b->hub ? "with several adapters on one capture" : "on a file source");
            return;
        }
        if (app_busy(b) && !force) {
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": false, \"error\": \"busy\"}");
            return;
        }
        /* the application's reception (forced) or a previous probe/monitor */
        if (b->mode)
            ctl_release(b);
        else if (b->mt)
            bridge_stop_rx(b);
        char ant[16];
        const char* ant0 = b->radio->antenna;
        if (json_str(req, "antenna", ant, sizeof(ant)) && ant[0])
            b->radio->antenna = ant;
        if (!strcmp(cmd, "capture")) {
            int n = (int)json_num(req, "samples", 65536);
            n = n < 1024 ? 1024 : n > 1048576 ? 1048576 : n;
            const char* gv = json_val(req, "gain");
            const double gain = gv && *gv == '"' ? -1 : json_num(req, "gain", -1);
            float* iq = malloc(sizeof(float) * 2 * n);
            double level = NAN, g = NAN;
            const int rc = iq ? radio_capture(b->radio, json_num(req, "freq", 0), json_num(req, "rate", 20e6), gain,
                                              json_num(req, "bandwidth", 0), n, iq, &level, &g)
                              : -1;
            const double f = b->radio->freq, rate = b->radio->rate;
            b->radio->antenna = ant0;
            pthread_mutex_unlock(&ctl_lock);
            if (rc) {
                sb_printf(o, "{\"ok\": false, \"error\": \"capture failed\"}");
            } else {
                sb_printf(o, "{\"ok\": true, \"freq\": %.0f, \"rate\": %.0f, \"gain\": %.1f, \"level_dbfs\": %.2f, \"iq\": \"", f,
                          rate, g, level);
                b64(o, (const uint8_t*)iq, sizeof(float) * 2 * (size_t)n);
                sb_printf(o, "\"}");
            }
            free(iq);
            return;
        }
        struct dvbrx_tune t;
        const double sr = json_num(req, "symbol_rate", 0);
        if (!strcmp(cmd, "monitor") && sr <= 0 && !strstr(req, "DVB-T")) {
            /* level only: the radio at the frequency, samples discarded */
            b->mode = 2;
            ctl_last_status = now();
            b->tune = (struct dvbrx_tune){ .delivery_system = SYS_DVBS2,
                                           .frequency_khz = (uint32_t)llround(json_num(req, "freq", 0) / 1e3) };
            b->radio->bw_need = 4e6;
            if (radio_configure(b->radio, json_num(req, "freq", 0), 8e6) > 0) {
                b->radio->sink = discard_sink;
                radio_start(b->radio, NULL);
            }
            b->radio->antenna = ant0;
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": true}");
            return;
        }
        if (tune_of(req, &t, err, sizeof(err))) {
            b->radio->antenna = ant0;
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": false, \"error\": \"%s\"}", err);
            return;
        }
        b->mode = !strcmp(cmd, "probe") ? 1 : 2;
        ctl_last_status = now(); /* the monitor watchdog counts from its start */
        b->psi = b->mode == 1 ? psi_new() : NULL;
        bridge_tune(b, &t);
        b->radio->antenna = ant0;
        const uint64_t gen = b->gen;
        const int ok = b->mt != NULL;
        pthread_mutex_unlock(&ctl_lock);
        if (!ok) {
            pthread_mutex_lock(&ctl_lock);
            ctl_release(b);
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": false, \"error\": \"tuning failed\"}");
            return;
        }
        if (b->mode == 2) {
            sb_printf(o, "{\"ok\": true}");
            return;
        }
        /* probe: let it lock and collect, then report */
        const double secs = fmax(1, fmin(60, json_num(req, "seconds", 6)));
        const double t_end = now() + secs;
        double t_lock = 0;
        uint64_t p_lock = 0;
        drx_rx_stats good = { 0 }; /* the last stats in lock (an AGC step may unlock at the end) */
        while (now() < t_end) {
            usleep(200000);
            pthread_mutex_lock(&ctl_lock);
            const int same = b->gen == gen && b->mode == 1;
            drx_rx_stats s = { 0 };
            if (same && b->mt)
                drx_mt_get_stats(b->mt, &s);
            pthread_mutex_unlock(&ctl_lock);
            if (!same)
                break;
            if (s.ts_packets > 0 && !t_lock) {
                t_lock = now();
                p_lock = s.ts_packets;
            }
            if (s.constellation && s.ts_packets > 0)
                good = s;
            /* locked and the tables seen for 3 s: enough */
            if (t_lock && now() - t_lock > 3.0 && b->radio->agc_state != 1)
                break;
        }
        pthread_mutex_lock(&ctl_lock);
        if (b->gen != gen || b->mode != 1) {
            pthread_mutex_unlock(&ctl_lock);
            sb_printf(o, "{\"ok\": false, \"error\": \"interrupted by the application\"}");
            return;
        }
        drx_rx_stats s;
        drx_mt_get_stats(b->mt, &s);
        if (!s.constellation && good.constellation) {
            s.constellation = good.constellation;
            s.rate = good.rate;
            s.snr_db = good.snr_db;
            s.freq = good.freq;
            s.pilots = good.pilots;
        }
        /* TS rate since the lock (the acquisition would lower it) */
        const int since_lock = t_lock && now() - t_lock > 1.0;
        const double t_rate = since_lock ? now() - t_lock : now() - b->t0;
        const uint64_t p_rate = since_lock ? p_lock : 0;
        const double gain = b->radio->gain, level = b->radio->level_dbfs;
        /* locked: a real TS rate (a DVB TS has a constant rate), not a few packets */
        const double ts_mbps = t_rate > 0 ? (s.ts_packets - p_rate) * 1504.0 / t_rate / 1e6 : 0;
        const int locked = s.ts_packets > 0 && ts_mbps > 1.0;
        const double off = s.freq * b->symrate;
        struct psi_result* pr = malloc(sizeof(*pr));
        if (pr)
            psi_result_get(b->psi, pr);
        /* PIDs seen: [pid, packets, scrambled packets] */
        struct sbuf pids = { 0 };
        int npid = 0;
        for (int pid = 0; pid < 8192; pid++) {
            uint32_t np, ns;
            psi_pid_count(b->psi, pid, &np, &ns);
            if (np)
                sb_printf(&pids, "%s[%d, %u, %u]", npid++ ? ", " : "", pid, np, ns);
        }
        const struct dvbrx_tune tt = b->tune;
        ctl_release(b);
        pthread_mutex_unlock(&ctl_lock);
        char rate[16] = "";
        if (s.rate) {
            snprintf(rate, sizeof(rate), "%s", s.rate);
            for (char* c = rate; *c; c++)
                if (*c == '_')
                    *c = '/';
        }
        sb_printf(o, "{\"ok\": true, \"locked\": %s, \"delsys\": \"%s\", \"symbol_rate\": %u, \"bandwidth\": %u, "
                     "\"rolloff\": %.2f, \"plp\": %d, ",
                  locked ? "true" : "false", delsys_name(tt.delivery_system), is_ofdm(&tt) ? 0 : tt.symbol_rate,
                  is_ofdm(&tt) ? tt.bandwidth_hz : 0,
                  is_ofdm(&tt) || tt.delivery_system == SYS_DVBC_ANNEX_A ? 0.0 : rolloff_of(&tt), tt.stream_id);
        if (tt.delivery_system == SYS_DVBS2)
            sb_printf(o, "\"pilots\": %s, ", s.pilots ? "true" : "false");
        sb_printf(o, "\"pids\": [%s], ", pids.s ? pids.s : "");
        free(pids.s);
        sb_num(o, "snr_db", s.snr_db > 0 ? s.snr_db : NAN, 2);
        sb_printf(o, ", ");
        sb_num(o, "freq_offset_hz", off, 0);
        sb_printf(o, ", \"modulation\": %s%s%s, \"code_rate\": %s%s%s, ", s.constellation ? "\"" : "",
                  s.constellation ? s.constellation : "null", s.constellation ? "\"" : "", rate[0] ? "\"" : "",
                  rate[0] ? rate : "null", rate[0] ? "\"" : "");
        sb_num(o, "ts_mbps", t_rate > 0 ? ts_mbps : NAN, 2);
        sb_printf(o, ", \"ts_packets\": %llu, \"ts_errors\": %llu, \"fec_blocks\": %llu, \"fec_errors\": %llu, ",
                  (unsigned long long)s.ts_packets, (unsigned long long)s.ts_crc_errors, (unsigned long long)s.plframes,
                  (unsigned long long)s.bch_failed);
        sb_num(o, "gain", gain, 1);
        sb_printf(o, ", ");
        sb_num(o, "level_dbfs", level, 1);
        if (pr) {
            sb_printf(o, ", \"tsid\": %d, \"onid\": %d, \"network\": \"%s\", \"services\": [", pr->tsid, pr->onid,
                      pr->network);
            for (int i = 0; i < pr->nservices; i++) {
                const struct psi_service* v = &pr->services[i];
                sb_printf(o, "%s{\"sid\": %d, \"name\": \"%s\", \"provider\": \"%s\", \"type\": %d, \"scrambled\": %s}",
                          i ? ", " : "", v->sid, v->name, v->provider, v->type, v->scrambled ? "true" : "false");
            }
            sb_printf(o, "], \"nit\": [");
            for (int i = 0; i < pr->nmuxes; i++) {
                const struct psi_mux* m = &pr->muxes[i];
                sb_printf(o, "%s{\"delsys\": \"%s\", \"freq\": %.0f, \"symbol_rate\": %.0f, \"bandwidth\": %.0f, "
                             "\"modulation\": \"%s%d\", \"tsid\": %d, \"plp\": %d, \"polarization\": \"%c\"}",
                          i ? ", " : "", m->delsys, m->freq, m->symbol_rate, m->bandwidth, m->modulation ? "QAM" : "",
                          m->modulation, m->tsid, m->plp, m->polarization ? m->polarization : '-');
            }
            sb_printf(o, "]");
            free(pr);
        }
        sb_printf(o, "}");
        return;
    }
    sb_printf(o, "{\"ok\": false, \"error\": \"unknown cmd \\\"%s\\\"\"}", cmd);
}

static void* ctl_client(void* arg)
{
    const int fd = (int)(intptr_t)arg;
    char* line = malloc(65536);
    size_t n = 0;
    while (line) {
        const ssize_t k = read(fd, line + n, 65535 - n);
        if (k <= 0)
            break;
        n += (size_t)k;
        char* nl;
        while ((nl = memchr(line, '\n', n))) {
            *nl = 0;
            struct sbuf o = { 0 };
            ctl_request(line, &o);
            sb_printf(&o, "\n");
            for (size_t w = 0; o.s && w < o.n;) {
                const ssize_t m = write(fd, o.s + w, o.n - w);
                if (m <= 0)
                    break;
                w += (size_t)m;
            }
            free(o.s);
            n -= (size_t)(nl + 1 - line);
            memmove(line, nl + 1, n);
        }
        if (n == 65535)
            n = 0; /* too long a line */
    }
    free(line);
    close(fd);
    return NULL;
}

static void* ctl_server(void* arg)
{
    const int lfd = (int)(intptr_t)arg;
    while (!stop) {
        const int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, ctl_client, (void*)(intptr_t)fd))
            close(fd);
        else
            pthread_detach(t);
    }
    return NULL;
}

/* Opens the control socket (0660, mode of the directory's group). */
static int ctl_open(const char* path, struct bridge* b, int n)
{
    ctl_b = b;
    ctl_n = n;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(a.sun_path))
        return -1;
    strcpy(a.sun_path, path);
    unlink(path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof(a)) || listen(fd, 8)) {
        perror(path);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    chmod(path, 0660);
    pthread_t t;
    pthread_create(&t, NULL, ctl_server, (void*)(intptr_t)fd);
    pthread_detach(t);
    fprintf(stderr, "control socket %s\n", path);
    return 0;
}

#define LOCK_HOLD 1.0 /* seconds */

static void bridge_status(struct bridge* b)
{
    struct dvbrx_status st = { .strength = INT_MIN };
    if (b->mt) {
        drx_rx_stats s;
        drx_mt_get_stats(b->mt, &s);
        /* Held for a while: short signal dropouts make the PL sync lose
         * and regain lock, which must not look like a lost transponder
         * (applications retune then). */
        const double t = now();
        if (s.plframes > b->last_frames)
            b->t_frames = t;
        if (s.ts_packets > b->last_packets)
            b->t_packets = t;
        b->last_frames = s.plframes;
        b->last_packets = s.ts_packets;
        if (t - b->t_frames < LOCK_HOLD)
            st.status = FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_SYNC;
        if (st.status && t - b->t_packets < LOCK_HOLD)
            st.status |= FE_HAS_VITERBI | FE_HAS_LOCK;
        st.cnr = (int32_t)(s.snr_db * 1000);
        st.block_count = s.plframes;
        st.block_errors = s.bch_failed;
    }
    if (ioctl(b->ctl, DVBRX_IOC_STATUS, &st))
        perror("DVBRX_IOC_STATUS");
}

static void bridge_event(struct bridge* b)
{
    struct dvbrx_event ev;
    if (read(b->ctl, &ev, sizeof(ev)) != sizeof(ev))
        return;
    if (ev.type == DVBRX_EV_TUNE) {
        pthread_mutex_lock(&ctl_lock);
        if (b->mode) /* the application comes first */
            ctl_release(b);
        bridge_tune(b, &ev.tune);
        pthread_mutex_unlock(&ctl_lock);
    } else if (ev.type == DVBRX_EV_SLEEP) {
        pthread_mutex_lock(&ctl_lock);
        if (!b->mode) { /* a probe or the monitor is not the application's */
            if (b->hub)
                fprintf(stderr, "adapter %d: ", b->index);
            fprintf(stderr, "frontend closed: stopping\n");
            bridge_stop_rx(b);
        }
        pthread_mutex_unlock(&ctl_lock);
    } else {
        struct dvbrx_reply r = { .seq = ev.seq, .result = lnb_event(b, &ev) };
        if (ioctl(b->ctl, DVBRX_IOC_REPLY, &r))
            perror("DVBRX_IOC_REPLY");
    }
}

/* Serves n adapters (one, or several sharing a hub) from this thread. */
static int run_bridges(struct bridge* b, int n)
{
    struct pollfd p[MAX_BRIDGES];
    for (int i = 0; i < n; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dvbrx%d", b[i].index);
        b[i].ctl = open(path, O_RDWR);
        if (b[i].ctl < 0) {
            perror(path);
            fprintf(stderr, "is the dvbrx module loaded (with adapters=%d or more)?\n",
                    b[i].index + 1);
            while (i--)
                close(b[i].ctl);
            return 1;
        }
        fprintf(stderr, "bridge: serving /dev/dvb through %s\n", path);
        b[i].last_print = b[i].t0 = now();
        b[i].ts_mbps = NAN;
    }
    if (ctl_path && ctl_path[0]) {
        char def[64];
        const char* cp = ctl_path;
        if (!strcmp(cp, "default")) {
            snprintf(def, sizeof(def), "/run/dvbrx/dvbrx-%d.sock", b[0].index);
            cp = def;
        }
        ctl_last_status = now();
        ctl_open(cp, b, n);
    }
    while (!stop) {
        for (int i = 0; i < n; i++)
            p[i] = (struct pollfd){ .fd = b[i].ctl, .events = POLLIN };
        if (poll(p, n, 100) > 0)
            for (int i = 0; i < n; i++)
                if (p[i].revents & POLLIN)
                    bridge_event(&b[i]);
        pthread_mutex_lock(&ctl_lock);
        for (int i = 0; i < n; i++) {
            if (now() - b[i].last_status >= 0.2) {
                if (!b[i].mode) /* the application sees its own reception only */
                    bridge_status(&b[i]);
                b[i].last_status = now();
            }
            if (b[i].mt && now() - b[i].last_print >= 2.0) {
                char tag[16] = "";
                if (b[i].hub)
                    snprintf(tag, sizeof(tag), "a%d ", b[i].index);
                print_stats(b[i].mt, b[i].radio, b[i].t0, b[i].symrate, tag);
                b[i].last_print = now();
            }
            /* TS rate for the control socket, over about 2 s */
            const double t = now();
            if (b[i].mt && t - b[i].rate_t >= 2.0) {
                drx_rx_stats s;
                drx_mt_get_stats(b[i].mt, &s);
                b[i].ts_mbps = b[i].rate_t > 0 && s.ts_packets >= b[i].rate_pk
                                   ? (s.ts_packets - b[i].rate_pk) * 1504.0 / (t - b[i].rate_t) / 1e6
                                   : NAN;
                b[i].rate_t = t;
                b[i].rate_pk = s.ts_packets;
            } else if (!b[i].mt) {
                b[i].rate_t = 0;
                b[i].ts_mbps = NAN;
            }
            /* a monitor nobody looks at any more */
            if (b[i].mode == 2 && t - ctl_last_status > 30) {
                fprintf(stderr, "monitor: no status request for 30 s, stopping\n");
                ctl_release(&b[i]);
            }
        }
        pthread_mutex_unlock(&ctl_lock);
    }
    for (int i = 0; i < n; i++) {
        bridge_stop_rx(&b[i]);
        close(b[i].ctl);
    }
    return 0;
}

/* ---- main ----------------------------------------------------------------- */

/* standalone -x: the channelizer between the source and the receiver */
struct standalone_chan {
    drx_mt* mt;
    int nonblock;
};

static void standalone_deliver(void* ctx, const float* iq, int n)
{
    struct standalone_chan* sc = ctx;
    if (sc->mt)
        drx_mt_push(sc->mt, iq, n, sc->nonblock);
}

static void drx_chan_process_cb(void* ctx, const float* iq, int n)
{
    drx_chan_process(ctx, iq, n);
}

/* ---- standalone, several channels (-m) ------------------------------------ */

#define MAX_MULTI 8

/* "f=HZ,sys=t2,bw=8,o=DEST" -> tuning request and output; 0 or -1 */
static int parse_multi(const char* spec, struct dvbrx_tune* t, double* freq, char* dest,
                       size_t destlen)
{
    memset(t, 0, sizeof(*t));
    t->delivery_system = SYS_DVBS2;
    t->stream_id = -1;
    t->rolloff = ROLLOFF_20;
    t->bandwidth_hz = 8000000;
    *freq = 0;
    dest[0] = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *save, *kv = strtok_r(buf, ",", &save); kv; kv = strtok_r(NULL, ",", &save)) {
        char* v = strchr(kv, '=');
        if (!v)
            return -1;
        *v++ = 0;
        if (!strcmp(kv, "f")) {
            *freq = atof(v);
        } else if (!strcmp(kv, "sys")) {
            if (!strcmp(v, "s")) t->delivery_system = SYS_DVBS;
            else if (!strcmp(v, "s2")) t->delivery_system = SYS_DVBS2;
            else if (!strcmp(v, "c")) t->delivery_system = SYS_DVBC_ANNEX_A;
            else if (!strcmp(v, "t")) t->delivery_system = SYS_DVBT;
            else if (!strcmp(v, "t2")) t->delivery_system = SYS_DVBT2;
            else return -1;
        } else if (!strcmp(kv, "sr")) {
            t->symbol_rate = (uint32_t)atof(v);
        } else if (!strcmp(kv, "bw")) {
            t->bandwidth_hz = (uint32_t)(atof(v) * 1e6);
        } else if (!strcmp(kv, "ro")) {
            const double r = atof(v);
            t->rolloff = r >= 0.3 ? ROLLOFF_35 : r >= 0.22 ? ROLLOFF_25 : r >= 0.17 ? ROLLOFF_20
                         : r >= 0.12 ? ROLLOFF_15 : r >= 0.07 ? ROLLOFF_10 : ROLLOFF_5;
        } else if (!strcmp(kv, "qam")) {
            const int q = atoi(v);
            t->modulation = q == 16 ? QAM_16 : q == 32 ? QAM_32 : q == 64 ? QAM_64
                            : q == 128 ? QAM_128 : q == 256 ? QAM_256 : QAM_AUTO;
        } else if (!strcmp(kv, "fec")) {
            const int r = parse_dvbs_rate(v);
            static const uint32_t fecs[] = { FEC_AUTO, FEC_1_2, FEC_2_3, FEC_3_4, FEC_5_6, FEC_7_8 };
            if (r < 0)
                return -1;
            t->fec_inner = fecs[r];
        } else if (!strcmp(kv, "isi") || !strcmp(kv, "plp")) {
            t->stream_id = atoi(v);
        } else if (!strcmp(kv, "gold")) {
            t->scrambling_index = (uint32_t)drx_pls_parse(v);
        } else if (!strcmp(kv, "o")) {
            snprintf(dest, destlen, "%s", v);
        } else {
            return -1;
        }
    }
    if (*freq <= 0 || !dest[0] || (!is_ofdm(t) && !t->symbol_rate))
        return -1;
    t->frequency_khz = (uint32_t)(*freq / 1e3);
    return 0;
}

/* Several channels of one capture (rate -S, centre -f or the middle of the
 * channels), each to its own output. */
static int run_multi(struct radio* radio, char** specs, int n, double centre, double rate,
                     int threads, int rrc_delay)
{
    struct dvbrx_tune t[MAX_MULTI];
    double f[MAX_MULTI], symrate[MAX_MULTI];
    struct output out[MAX_MULTI];
    struct standalone_chan sc[MAX_MULTI];
    drx_mt* mt[MAX_MULTI] = { 0 };
    char dest[MAX_MULTI][256];
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < n; i++) {
        if (parse_multi(specs[i], &t[i], &f[i], dest[i], sizeof(dest[i]))) {
            fprintf(stderr, "-m %s: expected f=HZ,sys=s|s2|c|t|t2,[sr=,bw=,ro=,qam=,fec=,isi=,"
                            "plp=,gold=,]o=DEST\n", specs[i]);
            return 2;
        }
        const struct chan_need need = need_of(&t[i]);
        lo = fmin(lo, f[i] - need.occ / 2);
        hi = fmax(hi, f[i] + need.occ / 2);
    }
    if (centre <= 0)
        centre = (lo + hi) / 2;
    if (rate <= 0)
        rate = 30.72e6;
    radio->bw_need = 2 * fmax(hi - centre, centre - lo); /* the channels asked for */
    if ((rate = radio_configure(radio, centre, rate)) <= 0)
        return 1;
    drx_chan* chan = drx_chan_new(rate);
    if (!chan)
        return 1;
    const struct bridge settings = { .threads = threads, .rrc_delay = rrc_delay };
    for (int i = 0; i < n; i++) {
        const struct chan_need need = need_of(&t[i]);
        double crate = 0;
        sc[i] = (struct standalone_chan){ .nonblock = radio->file_realtime || !radio->file };
        if (output_open(&out[i], dest[i], 0)) {
            fprintf(stderr, "cannot open output %s\n", dest[i]);
            return 1;
        }
        if (drx_chan_add(chan, f[i] - centre, need.occ, need.want, &crate, standalone_deliver,
                         &sc[i]) < 0 || crate < need.min) {
            fprintf(stderr, "channel %d (%.3f MHz) does not fit in the capture (%.3f MHz, %.3f "
                            "Msps)\n", i, f[i] / 1e6, centre / 1e6, rate / 1e6);
            return 2;
        }
        drx_mt_cfg cfg;
        rx_cfg_for(&settings, &t[i], crate, &cfg);
        if (!(mt[i] = drx_mt_new(&cfg, on_ts_output, &out[i]))) {
            fprintf(stderr, "channel %d: receiver setup failed\n", i);
            return 1;
        }
        sc[i].mt = mt[i];
        symrate[i] = is_ofdm(&t[i]) ? crate : t[i].symbol_rate;
        fprintf(stderr, "c%d: %.3f MHz (%+.3f in the capture), %.3f Msps -> %s\n", i, f[i] / 1e6,
                (f[i] - centre) / 1e6, crate / 1e6, dest[i]);
    }
    radio->sink = drx_chan_process_cb;
    radio->sink_ctx = chan;
    radio_start(radio, NULL);
    const double t0 = now();
    double last = t0;
    while (!stop && !(radio->file && radio->eof)) {
        usleep(20000);
        if (now() - last >= 1.0) {
            for (int i = 0; i < n; i++) {
                char tag[8];
                snprintf(tag, sizeof(tag), "c%d ", i);
                print_stats(mt[i], radio, t0, symrate[i], tag);
            }
            last = now();
        }
    }
    radio_close(radio);
    drx_chan_free(chan);
    for (int i = 0; i < n; i++) {
        char tag[8];
        snprintf(tag, sizeof(tag), "c%d ", i);
        drx_mt_finish(mt[i]);
        print_stats(mt[i], radio, t0, symrate[i], tag);
        drx_mt_free(mt[i]);
        if (out[i].file)
            fclose(out[i].file);
    }
    return 0;
}

static void usage(const char* prog)
{
    fprintf(stderr,
            "usage: %s (-u [DRIVER:]ARGS -f FREQ | -F FILE) -s SYMRATE [-S RATE] [-G GAIN] [-A ANT] "
            "[-M MCR] [-r ROLLOFF] [-D SYMBOLS] [-g GOLD] [-i ISI] [-l DVBS_RATE] [-c QAM] [-T MHZ] [-2 MHZ] [-o DEST] "
            "[-t THREADS] [-R]\n"
            "       %s -d N (-u [DRIVER:]ARGS | -F FILE) [-S MAX_RATE] [-G GAIN] [-A ANT] [-M MCR] "
            "[-L LNB] [-t THREADS]\n",
            prog, prog);
}

int main(int argc, char** argv)
{
    const char *dest = "-", *lnb_spec = "none", *dev_spec = NULL;
    struct radio radio = { .gain = 40, .bw_opt = -1 };
    double freq = 0, rate = 0, symrate = 0;
    float rolloff = 0.2f;
    int threads = 2, realtime = 0, gold = 0, isi = DRX_ISI_ALL, rrc_delay = 0, bridge = -1;
    int dvbs_rate = -1; /* -1: DVB-S2 */
    int qam = -1;       /* -1: not DVB-C */
    double dvbt_bw = 0;
    int dvbt2 = 0;
    int nbridges = 1;
    double wide_rate = 0, offset = NAN;
    char* multi[MAX_MULTI];
    int nmulti = 0;
    int opt;
    while ((opt = getopt(argc, argv, "u:F:f:S:G:A:B:C:P:M:s:r:D:g:i:o:t:Rd:L:l:c:T:2:n:W:x:m:")) != -1) {
        switch (opt) {
        case 'u': dev_spec = optarg; break;
        case 'F': radio.file = optarg; break;
        case 'f': freq = atof(optarg); break;
        case 'S': rate = atof(optarg); break;
        case 'G':
            radio.gain_auto = !strncmp(optarg, "auto", 4);
            radio.gain_cap = radio.gain_auto && optarg[4] == ':' ? atof(optarg + 5) : 0;
            radio.gain = radio.gain_auto ? (radio.gain_cap > 0 && radio.gain_cap < 30 ? radio.gain_cap : 30)
                                         : atof(optarg);
            break;
        case 'A': radio.antenna = optarg; break;
        case 'B': radio.bw_opt = atof(optarg); break;
        case 'C': ctl_path = optarg; break;
        case 'P': ctl_input_offset = atof(optarg); break;
        case 'M': radio.mcr = atof(optarg); break;
        case 's': symrate = atof(optarg); break;
        case 'r': rolloff = (float)atof(optarg); break;
        case 'D': rrc_delay = atoi(optarg); break;
        case 'g': gold = drx_pls_parse(optarg); break;
        case 'i': isi = atoi(optarg); break;
        case 'o': dest = optarg; break;
        case 't': threads = atoi(optarg); break;
        case 'R': realtime = 1; break;
        case 'd': bridge = atoi(optarg); break;
        case 'L': lnb_spec = optarg; break;
        case 'n': nbridges = atoi(optarg); break;
        case 'W': wide_rate = atof(optarg); break;
        case 'x': offset = atof(optarg); break;
        case 'm':
            if (nmulti == MAX_MULTI) {
                fprintf(stderr, "-m: at most %d channels\n", MAX_MULTI);
                return 2;
            }
            multi[nmulti++] = optarg;
            break;
        case 'l':
            if ((dvbs_rate = parse_dvbs_rate(optarg)) < 0) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'T': dvbt_bw = atof(optarg) * 1e6; break;
        case '2':
            dvbt_bw = atof(optarg) * 1e6;
            dvbt2 = 1;
            break;
        case 'c':
            qam = strcmp(optarg, "auto") ? atoi(optarg) : DRX_QAM_AUTO;
            rolloff = 0.15f;
            break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!dev_spec && !radio.file) {
        usage(argv[0]);
        return 2;
    }
    if (dev_spec && !radio.file && radio_select(&radio, dev_spec))
        return 2;
    if (gold < 0) {
        fprintf(stderr, "invalid PLS code\n");
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (bridge >= 0) {
        if (nbridges < 1 || nbridges > MAX_BRIDGES) {
            fprintf(stderr, "-n: 1 to %d adapters\n", MAX_BRIDGES);
            return 2;
        }
        struct lnb lnb;
        struct hub hub = { .radio = &radio };
        struct bridge b[MAX_BRIDGES];
        radio.file_loop = radio.file_realtime = 1;
        if (lnb_open(&lnb, lnb_spec))
            return 1;
        if ((nbridges > 1 || wide_rate > 0) && radio.drv && !radio.drv->multi) {
            fprintf(stderr, "several adapters on one capture (-n, -W): not with this SDR\n");
            return 2;
        }
        if (nbridges > 1 || wide_rate > 0) {
            /* one capture for all the adapters, at a fixed rate; the centre
             * follows the channels in use, or stays at -f */
            if (wide_rate <= 0)
                wide_rate = 30.72e6;
            hub.fixed_lo = freq > 0;
            radio.bw_need = 0; /* the whole capture */
            hub.fs = radio_configure(&radio, freq > 0 ? freq : 1e9, wide_rate);
            if (hub.fs <= 0)
                return 1;
            hub.lo = radio.freq;
            hub.n = nbridges;
            if (!(hub.chan = drx_chan_new(hub.fs)))
                return 1;
            fprintf(stderr, "%d adapters on one %.3f Msps capture%s\n", nbridges, hub.fs / 1e6,
                    hub.fixed_lo ? " (fixed centre)" : "");
        }
        for (int i = 0; i < nbridges; i++) {
            b[i] = (struct bridge){ .index = bridge + i, .radio = &radio, .lnb = &lnb,
                                    .hub = hub.n ? &hub : NULL, .threads = threads,
                                    .rrc_delay = rrc_delay,
                                    .max_rate = rate > 0 ? rate : radio.drv ? radio.drv->rate_max : 56e6,
                                    .ch_id = -1 };
            hub.b[i] = &b[i];
        }
        int ret = run_bridges(b, nbridges);
        radio_close(&radio);
        drx_chan_free(hub.chan);
        return ret;
    }

    if (nmulti) {
        if (radio.drv && !radio.drv->multi) {
            fprintf(stderr, "-m: several channels on one capture are not supported with this SDR\n");
            return 2;
        }
        radio.file_loop = radio.file_realtime = realtime;
        return run_multi(&radio, multi, nmulti, freq, rate, threads, rrc_delay);
    }
    if (dvbt_bw > 0)
        symrate = dvbt_bw * 8 / 7; /* the elementary rate, for the statistics */
    if (symrate <= 0) {
        usage(argv[0]);
        return 2;
    }
    if (rate <= 0)
        rate = dvbt_bw > 0 ? symrate * 1.12 : 2 * symrate;
    struct output out;
    if (output_open(&out, dest, isi == DRX_ISI_ALL)) {
        fprintf(stderr, "cannot open output %s\n", dest);
        return 1;
    }
    radio.file_loop = radio.file_realtime = realtime;
    /* one channel at the centre (not -x, where it sits in a wider capture) */
    radio.bw_need = !isnan(offset) ? 0
                    : dvbt_bw > 0  ? (dvbt2 ? 0.98 : 0.95) * dvbt_bw
                                   : symrate * (1 + (qam >= 0 ? 0.15 : rolloff));
    if ((rate = radio_configure(&radio, freq, rate)) <= 0)
        return 1;
    /* -x: -S is the rate of a wide capture, the channel is at -x Hz in it */
    struct standalone_chan sc = { .nonblock = realtime || !radio.file };
    drx_chan* chan = NULL;
    if (!isnan(offset)) {
        const double occ = dvbt_bw > 0 ? 0.98 * dvbt_bw : symrate * (1 + rolloff);
        const double want = dvbt_bw > 0 ? symrate * 1.12 : 2 * symrate;
        const double wide = rate;
        chan = drx_chan_new(wide);
        if (!chan || drx_chan_add(chan, offset, occ, want, &rate, standalone_deliver, &sc) < 0 ||
            rate < (dvbt_bw > 0 ? symrate : 1.5 * symrate)) {
            fprintf(stderr, "the channel does not fit in the capture\n");
            return 2;
        }
        fprintf(stderr, "channel at %+.3f MHz, %.3f Msps\n", offset / 1e6, rate / 1e6);
        radio.sink = drx_chan_process_cb;
        radio.sink_ctx = chan;
    }
    drx_mt_cfg cfg;
    drx_mt_cfg_default(&cfg, (float)(rate / symrate), rolloff);
    cfg.fe.rrc_delay = rrc_delay > 0 ? rrc_delay : rrc_delay_for(rolloff);
    cfg.rx.gold_code = gold;
    cfg.rx.isi = isi;
    cfg.ldpc_threads = threads;
    if (dvbs_rate >= 0) {
        cfg.dvbs = 1;
        cfg.dvbs_rate = dvbs_rate;
    }
    if (qam >= 0) {
        cfg.dvbc = 1;
        cfg.dvbc_qam = qam;
    }
    if (dvbt_bw > 0) {
        cfg.dvbt = !dvbt2;
        cfg.dvbt2 = dvbt2;
        cfg.sample_rate = rate;
        cfg.bandwidth = dvbt_bw;
        if (dvbt2 && isi == DRX_ISI_ALL)
            cfg.rx.isi = DRX_ISI_FIRST;
    }
    drx_mt* mt = drx_mt_new(&cfg, on_ts_output, &out);
    sc.mt = mt;
    if (!mt) {
        fprintf(stderr, "receiver setup failed (sps %.3f)\n", rate / symrate);
        return 1;
    }
    if (radio_start(&radio, mt)) {
        drx_mt_free(mt);
        return 1;
    }
    const double t0 = now();
    double last = t0;
    while (!stop && !(radio.file && radio.eof)) {
        usleep(20000);
        if (now() - last >= 1.0) {
            print_stats(mt, &radio, t0, dvbt_bw > 0 ? rate : symrate, "");
            last = now();
        }
    }
    radio_close(&radio);
    drx_chan_free(chan);
    drx_mt_finish(mt);
    print_stats(mt, &radio, t0, dvbt_bw > 0 ? rate : symrate, "");
    drx_mt_free(mt);
    if (out.file)
        fclose(out.file);
    return 0;
}
