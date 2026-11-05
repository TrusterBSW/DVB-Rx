/* UHD (USRP B200/B210 and clones) through the UHD C API. */
#include "sdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <uhd.h>

struct sdr {
    uhd_usrp_handle usrp;
    uhd_rx_streamer_handle rx;
    uhd_rx_metadata_handle md;
    size_t spb;
};

static void uhd_close(struct sdr* s)
{
    if (!s)
        return;
    if (s->md)
        uhd_rx_metadata_free(&s->md);
    if (s->rx)
        uhd_rx_streamer_free(&s->rx);
    if (s->usrp)
        uhd_usrp_free(&s->usrp);
    free(s);
}

static struct sdr* uhd_open(const char* args, double mcr, struct sdr_info* info)
{
    struct sdr* s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (uhd_usrp_make(&s->usrp, args) || uhd_rx_streamer_make(&s->rx) || uhd_rx_metadata_make(&s->md)) {
        fprintf(stderr, "UHD: cannot open \"%s\"\n", args);
        uhd_close(s);
        return NULL;
    }
    if (mcr > 0)
        uhd_usrp_set_master_clock_rate(s->usrp, mcr, 0);
    *info = (struct sdr_info){ .name = "UHD", .gain_min = 0, .gain_max = 60, .rate_max = 61.44e6,
                               .bw_min = 0.2e6, .bw_max = 56e6, .timestamps = 1 };
    uhd_meta_range_handle range;
    if (!uhd_meta_range_make(&range)) {
        if (!uhd_usrp_get_rx_gain_range(s->usrp, "", 0, range))
            uhd_meta_range_stop(range, &info->gain_max);
        uhd_meta_range_free(&range);
    }
    size_t chan = 0;
    uhd_stream_args_t sa = {
        .cpu_format = "fc32",
        .otw_format = "sc16",
        .args = "",
        .channel_list = &chan,
        .n_channels = 1,
    };
    if (uhd_usrp_get_rx_stream(s->usrp, &sa, s->rx)) {
        uhd_close(s);
        return NULL;
    }
    uhd_rx_streamer_max_num_samps(s->rx, &s->spb);
    return s;
}

static double uhd_set_rate(struct sdr* s, double rate)
{
    uhd_usrp_set_rx_rate(s->usrp, rate, 0);
    /* the AD9361's own AGC, as UHD sets it up, starves the ADC */
    uhd_usrp_set_rx_agc(s->usrp, false, 0);
    double got = rate;
    uhd_usrp_get_rx_rate(s->usrp, 0, &got);
    return got;
}

static double uhd_set_gain(struct sdr* s, double gain)
{
    uhd_usrp_set_rx_gain(s->usrp, gain, 0, "");
    double got = gain;
    uhd_usrp_get_rx_gain(s->usrp, 0, "", &got);
    return got;
}

static double uhd_set_bandwidth(struct sdr* s, double bw)
{
    uhd_usrp_set_rx_bandwidth(s->usrp, bw, 0);
    double got = bw;
    uhd_usrp_get_rx_bandwidth(s->usrp, 0, &got);
    return got;
}

static int uhd_set_antenna(struct sdr* s, const char* ant)
{
    return uhd_usrp_set_rx_antenna(s->usrp, ant, 0) ? -1 : 0;
}

static double uhd_set_freq(struct sdr* s, double freq)
{
    uhd_tune_request_t req = { .target_freq = freq,
                               .rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
                               .dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO };
    uhd_tune_result_t tune;
    uhd_usrp_set_rx_freq(s->usrp, &req, 0, &tune);
    return tune.actual_rf_freq;
}

static int uhd_start(struct sdr* s)
{
    uhd_stream_cmd_t cmd = { .stream_mode = UHD_STREAM_MODE_START_CONTINUOUS, .stream_now = true };
    return uhd_rx_streamer_issue_stream_cmd(s->rx, &cmd) ? -1 : 0;
}

static void uhd_stop(struct sdr* s)
{
    uhd_stream_cmd_t cmd = { .stream_mode = UHD_STREAM_MODE_STOP_CONTINUOUS, .stream_now = true };
    uhd_rx_streamer_issue_stream_cmd(s->rx, &cmd);
}

static int uhd_read(struct sdr* s, float* iq, int n, double timeout, double* t, enum sdr_status* st)
{
    void* p = iq;
    size_t got = 0;
    uhd_rx_streamer_recv(s->rx, &p, (size_t)n < s->spb ? (size_t)n : s->spb, &s->md, timeout, false, &got);
    *t = -1;
    bool has_time = false;
    uhd_rx_metadata_has_time_spec(s->md, &has_time);
    if (got > 0 && has_time) {
        int64_t full;
        double frac;
        uhd_rx_metadata_time_spec(s->md, &full, &frac);
        *t = (double)full + frac;
    }
    uhd_rx_metadata_error_code_t err;
    uhd_rx_metadata_error_code(s->md, &err);
    *st = err == UHD_RX_METADATA_ERROR_CODE_NONE       ? SDR_OK
          : err == UHD_RX_METADATA_ERROR_CODE_OVERFLOW ? SDR_OVERFLOW
          : err == UHD_RX_METADATA_ERROR_CODE_TIMEOUT  ? SDR_TIMEOUT
                                                       : SDR_ERROR;
    if (*st == SDR_ERROR)
        fprintf(stderr, "UHD receive error %d\n", (int)err);
    return (int)got;
}

const struct sdr_driver sdr_uhd = {
    .prefix = "uhd",
    .rate_max = 56e6,
    .multi = 1,
    .open = uhd_open,
    .set_rate = uhd_set_rate,
    .set_gain = uhd_set_gain,
    .set_bandwidth = uhd_set_bandwidth,
    .set_antenna = uhd_set_antenna,
    .set_freq = uhd_set_freq,
    .start = uhd_start,
    .stop = uhd_stop,
    .read = uhd_read,
    .close = uhd_close,
};
