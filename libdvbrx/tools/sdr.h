/*
 * Receive-only SDR drivers for dvbrx: one RX channel, interleaved fc32
 * samples (full scale 1.0). dvbrx keeps everything above them (stream
 * thread, gap filling, AGC, captures).
 */
#ifndef DVBRX_SDR_H
#define DVBRX_SDR_H

enum sdr_status {
    SDR_OK,
    SDR_OVERFLOW, /* samples were lost before (or instead of) these */
    SDR_TIMEOUT,  /* nothing came: the stream may have stopped */
    SDR_ERROR,
};

struct sdr_info {
    const char* name;          /* log prefix, e.g. "UHD" */
    double gain_min, gain_max; /* dB */
    double rate_max;           /* sample rate, Hz */
    double bw_min, bw_max;     /* analog filter, Hz */
    int timestamps;            /* reads give the device time: gaps can be filled */
};

struct sdr;

struct sdr_driver {
    const char* prefix; /* "-u PREFIX:ARGS" selects the driver ("uhd", "bladerf", "hackrf") */
    double rate_max;    /* sample rate, before the device is open */
    int multi;          /* one capture may serve several channels (hub, -m) */
    /* mcr: master clock rate (UHD), 0 for the driver's choice */
    struct sdr* (*open)(const char* args, double mcr, struct sdr_info* info);
    double (*set_rate)(struct sdr* s, double rate);    /* rate obtained */
    double (*set_gain)(struct sdr* s, double gain);    /* gain obtained */
    double (*set_bandwidth)(struct sdr* s, double bw); /* bandwidth obtained */
    int (*set_antenna)(struct sdr* s, const char* ant);
    double (*set_freq)(struct sdr* s, double freq);    /* RF frequency obtained */
    int (*start)(struct sdr* s);
    void (*stop)(struct sdr* s);
    /* Up to n samples; *t: device time of the first one (s), < 0 unknown.
     * Returns how many, with their status in *st. */
    int (*read)(struct sdr* s, float* iq, int n, double timeout, double* t, enum sdr_status* st);
    void (*close)(struct sdr* s);
};

#ifdef HAVE_UHD
extern const struct sdr_driver sdr_uhd;
#endif
#ifdef HAVE_BLADERF
extern const struct sdr_driver sdr_bladerf;
#endif
#ifdef HAVE_HACKRF
extern const struct sdr_driver sdr_hackrf;
#endif

#endif
