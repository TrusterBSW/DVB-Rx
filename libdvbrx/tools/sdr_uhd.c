/* UHD (USRP B200/B210 and clones) through the UHD C API. */
#include "sdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <uhd.h>
#include "sdr_dl.h"

/* the library's functions, loaded at the first open (sdr_dl.h) */
#define UHD_SYMS(X) \
    X(uhd_meta_range_free) \
    X(uhd_meta_range_make) \
    X(uhd_meta_range_stop) \
    X(uhd_rx_metadata_error_code) \
    X(uhd_rx_metadata_free) \
    X(uhd_rx_metadata_has_time_spec) \
    X(uhd_rx_metadata_make) \
    X(uhd_rx_metadata_time_spec) \
    X(uhd_rx_streamer_free) \
    X(uhd_rx_streamer_issue_stream_cmd) \
    X(uhd_rx_streamer_make) \
    X(uhd_rx_streamer_max_num_samps) \
    X(uhd_rx_streamer_recv) \
    X(uhd_usrp_free) \
    X(uhd_usrp_get_rx_bandwidth) \
    X(uhd_usrp_get_rx_gain) \
    X(uhd_usrp_get_rx_gain_range) \
    X(uhd_usrp_get_rx_info) \
    X(uhd_usrp_get_rx_rate) \
    X(uhd_usrp_get_rx_stream) \
    X(uhd_usrp_make) \
    X(uhd_usrp_rx_info_free) \
    X(uhd_usrp_set_master_clock_rate) \
    X(uhd_usrp_set_rx_agc) \
    X(uhd_usrp_set_rx_antenna) \
    X(uhd_usrp_set_rx_bandwidth) \
    X(uhd_usrp_set_rx_freq) \
    X(uhd_usrp_set_rx_gain) \
    X(uhd_usrp_set_rx_rate)
UHD_SYMS(DL_PTR)

static int uhd_load(void)
{
    static const char* const names[] = { "libuhd.so.4*", "libuhd.so", NULL };
    DL_LOAD(names, "UHD", UHD_SYMS, "install uhd-host or libuhd4.x")
}

#define uhd_meta_range_free (*dl_uhd_meta_range_free)
#define uhd_meta_range_make (*dl_uhd_meta_range_make)
#define uhd_meta_range_stop (*dl_uhd_meta_range_stop)
#define uhd_rx_metadata_error_code (*dl_uhd_rx_metadata_error_code)
#define uhd_rx_metadata_free (*dl_uhd_rx_metadata_free)
#define uhd_rx_metadata_has_time_spec (*dl_uhd_rx_metadata_has_time_spec)
#define uhd_rx_metadata_make (*dl_uhd_rx_metadata_make)
#define uhd_rx_metadata_time_spec (*dl_uhd_rx_metadata_time_spec)
#define uhd_rx_streamer_free (*dl_uhd_rx_streamer_free)
#define uhd_rx_streamer_issue_stream_cmd (*dl_uhd_rx_streamer_issue_stream_cmd)
#define uhd_rx_streamer_make (*dl_uhd_rx_streamer_make)
#define uhd_rx_streamer_max_num_samps (*dl_uhd_rx_streamer_max_num_samps)
#define uhd_rx_streamer_recv (*dl_uhd_rx_streamer_recv)
#define uhd_usrp_free (*dl_uhd_usrp_free)
#define uhd_usrp_get_rx_bandwidth (*dl_uhd_usrp_get_rx_bandwidth)
#define uhd_usrp_get_rx_gain (*dl_uhd_usrp_get_rx_gain)
#define uhd_usrp_get_rx_gain_range (*dl_uhd_usrp_get_rx_gain_range)
#define uhd_usrp_get_rx_info (*dl_uhd_usrp_get_rx_info)
#define uhd_usrp_get_rx_rate (*dl_uhd_usrp_get_rx_rate)
#define uhd_usrp_get_rx_stream (*dl_uhd_usrp_get_rx_stream)
#define uhd_usrp_make (*dl_uhd_usrp_make)
#define uhd_usrp_rx_info_free (*dl_uhd_usrp_rx_info_free)
#define uhd_usrp_set_master_clock_rate (*dl_uhd_usrp_set_master_clock_rate)
#define uhd_usrp_set_rx_agc (*dl_uhd_usrp_set_rx_agc)
#define uhd_usrp_set_rx_antenna (*dl_uhd_usrp_set_rx_antenna)
#define uhd_usrp_set_rx_bandwidth (*dl_uhd_usrp_set_rx_bandwidth)
#define uhd_usrp_set_rx_freq (*dl_uhd_usrp_set_rx_freq)
#define uhd_usrp_set_rx_gain (*dl_uhd_usrp_set_rx_gain)
#define uhd_usrp_set_rx_rate (*dl_uhd_usrp_set_rx_rate)

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
    if (uhd_load())
        return NULL;
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
                               .bw_min = 0.2e6, .bw_max = 56e6, .timestamps = 1,
                               .usb_bytes = 4 /* sc16 over the wire */ };
    uhd_usrp_rx_info_t ri;
    if (!uhd_usrp_get_rx_info(s->usrp, 0, &ri)) {
        snprintf(info->model, sizeof(info->model), "%s", ri.mboard_id ? ri.mboard_id : "");
        snprintf(info->serial, sizeof(info->serial), "%s", ri.mboard_serial ? ri.mboard_serial : "");
        uhd_usrp_rx_info_free(&ri);
    }
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
    .rate_max = 61.44e6,
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
