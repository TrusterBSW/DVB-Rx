/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Interface between the dvbrx kernel module and the receiver daemon.
 *
 * The module exposes a standard DVB adapter (/dev/dvb/adapterN) plus a
 * control device (/dev/dvbrxN) for the daemon:
 *   read()   returns struct dvbrx_event records (tuning, LNB control);
 *   write()  feeds MPEG-TS packets into the adapter's demux;
 *   ioctl()  DVBRX_IOC_STATUS reports lock and signal quality,
 *            DVBRX_IOC_REPLY completes a synchronous event (LNB control),
 *            DVBRX_IOC_NAME names the frontend after the SDR (FE_GET_INFO),
 *            DVBRX_IOC_LIMITS gives the symbol rates the SDR can receive.
 * poll() signals POLLIN when events are pending.
 */
#ifndef DVBRX_IOCTL_H
#define DVBRX_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum dvbrx_event_type {
	DVBRX_EV_TUNE = 1,    /* asynchronous: retune the receiver */
	DVBRX_EV_VOLTAGE = 2, /* synchronous: LNB supply, enum fe_sec_voltage */
	DVBRX_EV_TONE = 3,    /* synchronous: 22 kHz tone, enum fe_sec_tone_mode */
	DVBRX_EV_DISEQC = 4,  /* synchronous: DiSEqC master command */
	DVBRX_EV_BURST = 5,   /* synchronous: tone burst, enum fe_sec_mini_cmd */
	DVBRX_EV_SLEEP = 6,   /* asynchronous: frontend closed, stop receiving */
};

struct dvbrx_tune {
	__u32 delivery_system; /* enum fe_delivery_system */
	__u32 frequency_khz;   /* satellite: intermediate frequency after the
				* LNB; cable, terrestrial: RF frequency */
	__u32 symbol_rate;     /* symbols per second */
	__u32 modulation;      /* enum fe_modulation (QPSK, PSK_8, APSK_16, ...) */
	__u32 fec_inner;       /* enum fe_code_rate (DVB-T: high priority) */
	__u32 rolloff;         /* enum fe_rolloff */
	__u32 pilot;           /* enum fe_pilot */
	__u32 inversion;       /* enum fe_spectral_inversion */
	__s32 stream_id;       /* ISI / PLP id, or -1 (NO_STREAM_ID_FILTER) */
	__u32 scrambling_index; /* PL scrambling gold code */
	/* DVB-T / DVB-T2 */
	__u32 bandwidth_hz;
	__u32 transmission_mode; /* enum fe_transmit_mode */
	__u32 guard_interval;    /* enum fe_guard_interval */
	__u32 hierarchy;         /* enum fe_hierarchy */
	__u32 code_rate_lp;      /* enum fe_code_rate */
};

struct dvbrx_event {
	__u32 type; /* enum dvbrx_event_type */
	__u32 seq;  /* to quote in DVBRX_IOC_REPLY for synchronous events */
	union {
		struct dvbrx_tune tune;
		__u32 voltage;
		__u32 tone;
		__u32 burst;
		struct {
			__u8 len;
			__u8 msg[6];
		} diseqc;
		__u8 pad[64];
	};
};

struct dvbrx_status {
	__u32 status; /* enum fe_status bits (FE_HAS_LOCK, ...) */
	__s32 cnr;    /* carrier to noise, 0.001 dB */
	__s32 strength; /* signal strength, 0.001 dBm, or INT_MIN if unknown */
	__u32 reserved;
	__u64 bit_errors; /* post-FEC counters, cumulative */
	__u64 bit_count;
	__u64 block_errors;
	__u64 block_count;
};

struct dvbrx_reply {
	__u32 seq;
	__s32 result; /* 0 or a negative errno for the application */
};

/* The frontend's name, e.g. "dvbrx B210 31A5B7C": applications such as
 * tvheadend read it when they find the adapter, and tell the tuners apart
 * by it. It stays after the daemon closes /dev/dvbrxN. */
struct dvbrx_name {
	char name[128]; /* NUL-terminated */
};

/* Symbol rates the SDR can receive (FE_GET_INFO; 0: unchanged). dvb-core
 * refuses a DVB-S/S2/C tuning outside them with EINVAL, at once: the
 * application (tvheadend) then tries another tuner instead of waiting for
 * a lock that cannot come. */
struct dvbrx_limits {
	__u32 symbol_rate_min;
	__u32 symbol_rate_max;
	__u32 reserved[6];
};

#define DVBRX_IOC_STATUS _IOW('S', 1, struct dvbrx_status)
#define DVBRX_IOC_REPLY _IOW('S', 2, struct dvbrx_reply)
#define DVBRX_IOC_NAME _IOW('S', 3, struct dvbrx_name)
#define DVBRX_IOC_LIMITS _IOW('S', 4, struct dvbrx_limits)

#endif /* DVBRX_IOCTL_H */
