/* bladeRF (libbladeRF): bladeRF 2.0 micro (AD9361) and bladeRF x40/x115.
 *
 * Sync interface, SC16 Q11 samples (full scale 2048). With metadata (the
 * timestamp of the first sample, an overrun flag) dvbrx fills the gaps as
 * with UHD. But a bladeRF 2.0 micro xA4 shipped with FPGA 0.16.0, newer than
 * libbladeRF 2024.05 knows, streamed only a quarter of the samples, the
 * metadata reads coming one 508-sample message at a time, each flagged as an
 * overrun (FPGA 0.15.3 streams them all): with an FPGA newer than 0.15 the
 * plain format is used, without timestamps. DRX_BLADERF_META=0/1 forces it.
 * The gain set while the input is off is only applied when it is enabled
 * (and reads back wrong until then): it is set again at the start.
 * Antennas: "RX1" (default) or "RX2", the board's two inputs. */
#include "sdr.h"

#include <libbladeRF.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct sdr {
    struct bladerf* dev;
    bladerf_channel ch;
    int streaming;
    double rate, gain, bw; /* last set, to carry over to the other input */
    double gain_asked;     /* applied again when the input is enabled */
    int meta;              /* metadata stream (timestamps) */
    int16_t* buf;          /* SC16 Q11 */
    int nbuf;
};

enum { BUF_SAMPLES = 16384, NUM_BUFFERS = 64, NUM_TRANSFERS = 16, STREAM_TIMEOUT_MS = 3500 };

static void brf_close(struct sdr* s)
{
    if (!s)
        return;
    if (s->dev) {
        if (s->streaming)
            bladerf_enable_module(s->dev, s->ch, false);
        bladerf_close(s->dev);
    }
    free(s->buf);
    free(s);
}

static int brf_check(int rc, const char* what)
{
    if (rc)
        fprintf(stderr, "bladeRF: %s: %s\n", what, bladerf_strerror(rc));
    return rc;
}

static double brf_set_rate(struct sdr* s, double rate)
{
    /* rational rate: the receivers resample from whatever rate they get */
    struct bladerf_rational_rate want = { .integer = (uint64_t)floor(rate), .den = 1000000 };
    want.num = (uint64_t)llround((rate - (double)want.integer) * 1e6);
    if (want.num >= want.den) {
        want.integer++;
        want.num = 0;
    }
    struct bladerf_rational_rate got = want;
    if (brf_check(bladerf_set_rational_sample_rate(s->dev, s->ch, &want, &got), "sample rate"))
        return rate;
    s->rate = (double)got.integer + (got.den ? (double)got.num / (double)got.den : 0);
    return s->rate;
}

static double brf_set_gain(struct sdr* s, double gain)
{
    s->gain_asked = gain;
    brf_check(bladerf_set_gain(s->dev, s->ch, (bladerf_gain)lround(gain)), "gain");
    bladerf_gain got = (bladerf_gain)lround(gain);
    if (s->streaming) /* off, the board reads back its default */
        bladerf_get_gain(s->dev, s->ch, &got);
    s->gain = got;
    return got;
}

static double brf_set_bandwidth(struct sdr* s, double bw)
{
    bladerf_bandwidth got = (bladerf_bandwidth)lround(bw);
    if (brf_check(bladerf_set_bandwidth(s->dev, s->ch, (bladerf_bandwidth)lround(bw), &got), "bandwidth"))
        return bw;
    s->bw = got;
    return got;
}

static struct sdr* brf_open(const char* args, double mcr, struct sdr_info* info)
{
    (void)mcr;
    struct sdr* s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ch = BLADERF_CHANNEL_RX(0);
    if (brf_check(bladerf_open(&s->dev, args && *args ? args : NULL), "open")) {
        fprintf(stderr, "bladeRF: cannot open \"%s\"\n", args ? args : "");
        s->dev = NULL;
        brf_close(s);
        return NULL;
    }
    brf_check(bladerf_set_gain_mode(s->dev, s->ch, BLADERF_GAIN_MGC), "manual gain");
    struct bladerf_version fv = { 0 };
    bladerf_fpga_version(s->dev, &fv);
    s->meta = fv.major == 0 && fv.minor <= 15;
    const char* e = getenv("DRX_BLADERF_META");
    if (e && *e)
        s->meta = *e != '0';
    *info = (struct sdr_info){ .name = "bladeRF", .gain_min = 0, .gain_max = 60, .rate_max = 61.44e6,
                               .bw_min = 0.2e6, .bw_max = 56e6, .timestamps = s->meta };
    const struct bladerf_range* r;
    if (!bladerf_get_gain_range(s->dev, s->ch, &r)) {
        info->gain_min = r->min * r->scale;
        info->gain_max = r->max * r->scale;
    }
    if (!bladerf_get_sample_rate_range(s->dev, s->ch, &r))
        info->rate_max = r->max * r->scale;
    if (!bladerf_get_bandwidth_range(s->dev, s->ch, &r)) {
        info->bw_min = r->min * r->scale;
        info->bw_max = r->max * r->scale;
    }
    bladerf_fpga_size fs = BLADERF_FPGA_UNKNOWN;
    bladerf_get_fpga_size(s->dev, &fs);
    const char* board = bladerf_get_board_name(s->dev);
    snprintf(info->model, sizeof(info->model), "%s%s",
             !strcmp(board, "bladerf2") ? "bladeRF 2.0" : !strcmp(board, "bladerf1") ? "bladeRF" : board,
             fs == BLADERF_FPGA_A4 ? " xA4" : fs == BLADERF_FPGA_A5 ? " xA5" : fs == BLADERF_FPGA_A9 ? " xA9" :
             fs == BLADERF_FPGA_40KLE ? " x40" : fs == BLADERF_FPGA_115KLE ? " x115" : "");
    struct bladerf_serial sn;
    if (!bladerf_get_serial_struct(s->dev, &sn))
        snprintf(info->serial, sizeof(info->serial), "%s", sn.serial);
    fprintf(stderr, "bladeRF: %s, serial %s, FPGA %u.%u.%u, gain %.0f..%.0f dB, up to %.2f Msps, filter %.2f..%.2f MHz, %s\n",
            info->model, info->serial[0] ? info->serial : "?", fv.major, fv.minor, fv.patch, info->gain_min, info->gain_max,
            info->rate_max / 1e6, info->bw_min / 1e6, info->bw_max / 1e6,
            s->meta ? "timestamps" : "no timestamps");
    return s;
}

static int brf_set_antenna(struct sdr* s, const char* ant)
{
    bladerf_channel ch;
    if (!strcasecmp(ant, "RX1") || !strcasecmp(ant, "RX"))
        ch = BLADERF_CHANNEL_RX(0);
    else if (!strcasecmp(ant, "RX2"))
        ch = BLADERF_CHANNEL_RX(1);
    else
        return -1;
    if (ch == s->ch)
        return 0;
    if (s->streaming)
        return -1;
    s->ch = ch;
    /* the settings are per input: carry them over */
    brf_check(bladerf_set_gain_mode(s->dev, s->ch, BLADERF_GAIN_MGC), "manual gain");
    if (s->rate > 0)
        brf_set_rate(s, s->rate);
    if (s->bw > 0)
        brf_set_bandwidth(s, s->bw);
    brf_set_gain(s, s->gain);
    return 0;
}

static double brf_set_freq(struct sdr* s, double freq)
{
    if (brf_check(bladerf_set_frequency(s->dev, s->ch, (bladerf_frequency)llround(freq)), "frequency"))
        return freq;
    bladerf_frequency got = (bladerf_frequency)llround(freq);
    bladerf_get_frequency(s->dev, s->ch, &got);
    return (double)got;
}

static int brf_start(struct sdr* s)
{
    if (s->streaming)
        return 0;
    if (brf_check(bladerf_sync_config(s->dev, BLADERF_RX_X1, s->meta ? BLADERF_FORMAT_SC16_Q11_META : BLADERF_FORMAT_SC16_Q11,
                                      NUM_BUFFERS, BUF_SAMPLES, NUM_TRANSFERS, STREAM_TIMEOUT_MS),
                  "stream setup") ||
        brf_check(bladerf_enable_module(s->dev, s->ch, true), "start"))
        return -1;
    s->streaming = 1;
    brf_set_gain(s, s->gain_asked);
    if (s->bw > 0) /* set while the input was off: again, to be sure */
        brf_set_bandwidth(s, s->bw);
    if (getenv("DRX_BLADERF_DEBUG")) {
        bladerf_bandwidth bw = 0;
        bladerf_sample_rate sr = 0;
        bladerf_get_bandwidth(s->dev, s->ch, &bw);
        bladerf_get_sample_rate(s->dev, s->ch, &sr);
        fprintf(stderr, "bladeRF: started, filter %.2f MHz, %.6f Msps\n", bw / 1e6, sr / 1e6);
    }
    return 0;
}

static void brf_stop(struct sdr* s)
{
    if (!s->streaming)
        return;
    bladerf_enable_module(s->dev, s->ch, false);
    s->streaming = 0;
}

static int brf_read(struct sdr* s, float* iq, int n, double timeout, double* t, enum sdr_status* st)
{
    *t = -1;
    if (!s->streaming) {
        *st = SDR_TIMEOUT;
        return 0;
    }
    if (n > s->nbuf) {
        int16_t* b = realloc(s->buf, sizeof(int16_t) * 2 * (size_t)n);
        if (!b) {
            *st = SDR_ERROR;
            return 0;
        }
        s->buf = b;
        s->nbuf = n;
    }
    struct bladerf_metadata md;
    memset(&md, 0, sizeof(md));
    md.flags = BLADERF_META_FLAG_RX_NOW;
    const int rc = bladerf_sync_rx(s->dev, s->buf, (unsigned)n, s->meta ? &md : NULL, (unsigned)lround(timeout * 1000));
    if (rc == BLADERF_ERR_TIMEOUT) {
        *st = SDR_TIMEOUT;
        return 0;
    }
    if (rc) {
        fprintf(stderr, "bladeRF receive error: %s\n", bladerf_strerror(rc));
        *st = SDR_ERROR;
        return 0;
    }
    const int got = !s->meta ? n : (int)md.actual_count < n ? (int)md.actual_count : n;
    for (int i = 0; i < 2 * got; i++)
        iq[i] = s->buf[i] * (1.0f / 2048);
    if (s->meta && s->rate > 0)
        *t = (double)md.timestamp / s->rate;
    *st = s->meta && (md.status & BLADERF_META_STATUS_OVERRUN) ? SDR_OVERFLOW : SDR_OK;
    return got;
}

const struct sdr_driver sdr_bladerf = {
    .prefix = "bladerf",
    .rate_max = 56e6,
    .multi = 1,
    .open = brf_open,
    .set_rate = brf_set_rate,
    .set_gain = brf_set_gain,
    .set_bandwidth = brf_set_bandwidth,
    .set_antenna = brf_set_antenna,
    .set_freq = brf_set_freq,
    .start = brf_start,
    .stop = brf_stop,
    .read = brf_read,
    .close = brf_close,
};
