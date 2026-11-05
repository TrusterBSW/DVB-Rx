/* HackRF One (libhackrf).
 *
 * - Samples: 8-bit I/Q from libhackrf's own thread, through a ring to the
 *   reader; a full ring (the reader late) drops and is reported as an
 *   overflow. No timestamps: dvbrx cannot fill the gaps.
 * - The strong DC component of the HackRF is taken out by a DC blocker
 *   (about 300 Hz wide) rather than by tuning off-centre, which would cost
 *   bandwidth out of the 20 MS/s.
 * - One gain, 0 to 116 dB, filled from the front: the 14 dB amplifier,
 *   then the LNA (0-40 dB by 8), then the VGA (0-62 dB by 2).
 * - The antenna port power (bias tee) is forced off at every open and
 *   start: it would put DC on the cable.
 * - Overruns of the HackRF's own buffers are counted by its M0 core
 *   (num_shortfalls), read at most every 0.25 s. */
#include "sdr.h"

#include <libhackrf/hackrf.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define RING (1u << 24) /* bytes: 8 M samples, 0.4 s at 20 MS/s */
#define DC_ALPHA 1e-4f  /* DC blocker: about 300 Hz at 20 MS/s */

struct sdr {
    hackrf_device* dev;
    double rate;
    int streaming;
    /* ring of 8-bit I/Q, written by libhackrf's thread */
    int8_t* ring;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    size_t head, tail; /* bytes written / read, free running */
    int dropped;       /* the ring overflowed since the last read */
    /* DC blocker */
    float dci, dcq;
    /* M0 overrun counter */
    uint32_t shortfalls;
    double t_check;
};

static int hackrf_users; /* hackrf_init / hackrf_exit pairs */

static double mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

static int hrf_check(int rc, const char* what)
{
    if (rc != HACKRF_SUCCESS)
        fprintf(stderr, "HackRF: %s: %s\n", what, hackrf_error_name((enum hackrf_error)rc));
    return rc != HACKRF_SUCCESS;
}

static void hrf_close(struct sdr* s)
{
    if (!s)
        return;
    if (s->dev) {
        if (s->streaming)
            hackrf_stop_rx(s->dev);
        hackrf_set_antenna_enable(s->dev, 0);
        hackrf_close(s->dev);
        if (--hackrf_users == 0)
            hackrf_exit();
    }
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s->ring);
    free(s);
}

static struct sdr* hrf_open(const char* args, double mcr, struct sdr_info* info)
{
    (void)mcr;
    struct sdr* s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->ring = malloc(RING);
    if (!s->ring) {
        hrf_close(s);
        return NULL;
    }
    if (hackrf_users++ == 0 && hrf_check(hackrf_init(), "init")) {
        hackrf_users--;
        hrf_close(s);
        return NULL;
    }
    /* args: a serial number (or its end), "" for the first HackRF */
    const char* serial = args && *args ? args : NULL;
    if (serial && !strncmp(serial, "serial=", 7))
        serial += 7;
    if (hrf_check(serial ? hackrf_open_by_serial(serial, &s->dev) : hackrf_open(&s->dev), "open")) {
        fprintf(stderr, "HackRF: cannot open \"%s\"\n", args ? args : "");
        s->dev = NULL;
        if (--hackrf_users == 0)
            hackrf_exit();
        hrf_close(s);
        return NULL;
    }
    hackrf_set_antenna_enable(s->dev, 0);
    uint8_t board = 0;
    hackrf_board_id_read(s->dev, &board);
    *info = (struct sdr_info){ .name = "HackRF", .gain_min = 0, .gain_max = 116, .rate_max = 20e6,
                               .bw_min = 1.75e6, .bw_max = 28e6, .timestamps = 0 };
    fprintf(stderr, "HackRF: %s, gain 0..116 dB (amp 14 + LNA 40 + VGA 62), up to 20 Msps\n",
            hackrf_board_id_name((enum hackrf_board_id)board));
    return s;
}

static double hrf_set_rate(struct sdr* s, double rate)
{
    /* below 2 MS/s the MAX2837's filters do not follow: the receivers
     * resample from what they get */
    if (rate < 2e6)
        rate = 2e6;
    if (rate > 20e6)
        rate = 20e6;
    if (hrf_check(hackrf_set_sample_rate(s->dev, rate), "sample rate"))
        return s->rate > 0 ? s->rate : rate;
    s->rate = rate;
    return rate;
}

static double hrf_set_gain(struct sdr* s, double gain)
{
    int g = (int)lround(gain);
    g = g < 0 ? 0 : g > 116 ? 116 : g;
    /* Gain from the front: the amplifier, then the LNA, then the VGA. The
     * HackRF's noise figure is poor without its front stages, and the
     * signals it is used on (cable channels around -75 dBm behind a
     * splitter) are noise limited: with the amplifier off and the LNA at
     * its share, a DVB-C channel read 18 dB of MER instead of 26. The AGC's
     * best-SNR search steps back if the front stages compress. */
    const int amp = g >= 14;
    const int rest = g - 14 * amp;
    const int lna = rest >= 40 ? 40 : rest / 8 * 8;
    int vga = (rest - lna + 1) / 2 * 2;
    vga = vga > 62 ? 62 : vga;
    hrf_check(hackrf_set_amp_enable(s->dev, (uint8_t)amp), "amplifier");
    hrf_check(hackrf_set_lna_gain(s->dev, (uint32_t)lna), "LNA gain");
    hrf_check(hackrf_set_vga_gain(s->dev, (uint32_t)vga), "VGA gain");
    return 14 * amp + lna + vga;
}

static double hrf_set_bandwidth(struct sdr* s, double bw)
{
    /* the MAX2837's settings: the first one not narrower than asked, so
     * that the channel is never cut */
    static const uint32_t BW[] = { 1750000,  2500000,  3500000,  5000000,  5500000,  6000000,
                                   7000000,  8000000,  9000000,  10000000, 12000000, 14000000,
                                   15000000, 20000000, 24000000, 28000000 };
    uint32_t pick = BW[sizeof(BW) / sizeof(BW[0]) - 1];
    for (size_t i = 0; i < sizeof(BW) / sizeof(BW[0]); i++)
        if (BW[i] >= bw) {
            pick = BW[i];
            break;
        }
    if (hrf_check(hackrf_set_baseband_filter_bandwidth(s->dev, pick), "bandwidth"))
        return bw;
    return pick;
}

static int hrf_set_antenna(struct sdr* s, const char* ant)
{
    (void)s;
    /* one input; "RX" or "ANT" accepted, the port power stays off */
    return !strcasecmp(ant, "RX") || !strcasecmp(ant, "ANT") ? 0 : -1;
}

static double hrf_set_freq(struct sdr* s, double freq)
{
    if (hrf_check(hackrf_set_freq(s->dev, (uint64_t)llround(freq)), "frequency"))
        return freq;
    return (double)(uint64_t)llround(freq);
}

/* libhackrf's thread: append to the ring, drop when it is full. */
static int rx_cb(hackrf_transfer* t)
{
    struct sdr* s = t->rx_ctx;
    const size_t n = (size_t)t->valid_length & ~(size_t)1;
    pthread_mutex_lock(&s->lock);
    if (s->head - s->tail + n > RING) {
        s->dropped = 1;
    } else {
        const size_t at = s->head & (RING - 1), first = RING - at < n ? RING - at : n;
        memcpy(s->ring + at, t->buffer, first);
        memcpy(s->ring, t->buffer + first, n - first);
        s->head += n;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}

static int hrf_start(struct sdr* s)
{
    if (s->streaming)
        return 0;
    hackrf_set_antenna_enable(s->dev, 0);
    pthread_mutex_lock(&s->lock);
    s->head = s->tail = 0;
    s->dropped = 0;
    pthread_mutex_unlock(&s->lock);
    hackrf_m0_state m0;
    s->shortfalls = !hackrf_get_m0_state(s->dev, &m0) ? m0.num_shortfalls : 0;
    s->t_check = mono();
    if (hrf_check(hackrf_start_rx(s->dev, rx_cb, s), "start"))
        return -1;
    s->streaming = 1;
    return 0;
}

static void hrf_stop(struct sdr* s)
{
    if (!s->streaming)
        return;
    hackrf_stop_rx(s->dev);
    s->streaming = 0;
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static int hrf_read(struct sdr* s, float* iq, int n, double timeout, double* t, enum sdr_status* st)
{
    *t = -1;
    *st = SDR_OK;
    pthread_mutex_lock(&s->lock);
    if (s->head == s->tail && s->streaming) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        const double end = dl.tv_sec + 1e-9 * dl.tv_nsec + timeout;
        dl.tv_sec = (time_t)end;
        dl.tv_nsec = (long)((end - floor(end)) * 1e9);
        while (s->head == s->tail && s->streaming)
            if (pthread_cond_timedwait(&s->cond, &s->lock, &dl))
                break;
    }
    size_t avail = (s->head - s->tail) / 2;
    if ((size_t)n > avail)
        n = (int)avail;
    const size_t at = s->tail & (RING - 1);
    const int8_t* p = s->ring + at;
    const size_t first = (RING - at) / 2 < (size_t)n ? (RING - at) / 2 : (size_t)n;
    /* convert with the DC blocker, the ring may wrap once */
    float dci = s->dci, dcq = s->dcq;
    for (size_t k = 0; k < (size_t)n; k++) {
        if (k == first)
            p = s->ring - 2 * first;
        const float i = p[2 * k] * (1.0f / 128), q = p[2 * k + 1] * (1.0f / 128);
        dci += DC_ALPHA * (i - dci);
        dcq += DC_ALPHA * (q - dcq);
        iq[2 * k] = i - dci;
        iq[2 * k + 1] = q - dcq;
    }
    s->dci = dci;
    s->dcq = dcq;
    s->tail += 2 * (size_t)n;
    const int dropped = s->dropped;
    s->dropped = 0;
    pthread_mutex_unlock(&s->lock);
    if (dropped)
        *st = SDR_OVERFLOW;
    /* the HackRF's own overruns */
    const double now = mono();
    if (s->streaming && now - s->t_check > 0.25) {
        s->t_check = now;
        hackrf_m0_state m0;
        if (!hackrf_get_m0_state(s->dev, &m0) && m0.num_shortfalls != s->shortfalls) {
            s->shortfalls = m0.num_shortfalls;
            *st = SDR_OVERFLOW;
        }
    }
    if (n == 0 && *st == SDR_OK)
        *st = SDR_TIMEOUT;
    return n;
}

const struct sdr_driver sdr_hackrf = {
    .prefix = "hackrf",
    .rate_max = 20e6,
    .multi = 0,
    .open = hrf_open,
    .set_rate = hrf_set_rate,
    .set_gain = hrf_set_gain,
    .set_bandwidth = hrf_set_bandwidth,
    .set_antenna = hrf_set_antenna,
    .set_freq = hrf_set_freq,
    .start = hrf_start,
    .stop = hrf_stop,
    .read = hrf_read,
    .close = hrf_close,
};
