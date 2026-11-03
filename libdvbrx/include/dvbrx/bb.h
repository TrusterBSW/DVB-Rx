/*
 * DVB-S2 baseband (BB) layer: BBFRAME descrambling, BBHEADER parsing and
 * MPEG-TS packet reconstruction (EN 302 307-1, sections 5.1 and 5.2).
 *
 * Reconstruction state is kept per input stream identifier (ISI), so the
 * streams of a multistream (MIS) signal can all be recovered at once.
 */
#ifndef DVBRX_BB_H
#define DVBRX_BB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRX_TS_PACKET_LEN 188
#define DRX_BBHEADER_LEN 10 /* bytes */

enum drx_ts_gs {
    DRX_GS_PACKETIZED = 0,
    DRX_GS_CONTINUOUS = 1,
    DRX_GS_RESERVED = 2,
    DRX_TS = 3,
};

typedef struct {
    uint8_t ts_gs;   /* enum drx_ts_gs */
    uint8_t sis;     /* 1: single input stream, 0: multiple */
    uint8_t ccm;     /* 1: constant coding and modulation, 0: ACM/VCM */
    uint8_t issyi;   /* input stream synchronisation indicator */
    uint8_t npd;     /* null-packet deletion active */
    uint8_t ro;      /* roll-off code (0: 0.35, 1: 0.25, 2: 0.20, 3: reserved) */
    uint8_t isi;     /* input stream identifier (0 for SIS) */
    uint16_t upl;    /* user packet length, bits */
    uint16_t dfl;    /* data field length, bits */
    uint8_t sync;    /* user packet sync byte */
    uint16_t syncd;  /* bits from the start of the data field to the first UP */
} drx_bbheader;

/* CRC-8 used by the BBHEADER and the TS packets (x^8+x^7+x^6+x^4+x^2+1). */
uint8_t drx_crc8(const uint8_t* data, int len);

/* Descramble a packed (MSB first) BBFRAME of kbch_bytes in place. */
void drx_bb_descramble(uint8_t* frame, int kbch_bytes);

/* Parse and check a BBHEADER. Returns 0 if valid, -1 otherwise. */
int drx_bbheader_parse(const uint8_t* frame, drx_bbheader* h);

typedef struct drx_bbdec drx_bbdec;

typedef struct {
    uint64_t frames;      /* BBFRAMEs seen */
    uint64_t bad_headers; /* dropped on a bad BBHEADER */
    uint64_t gaps;        /* resynchronisations after a discontinuity */
    uint64_t packets;     /* TS packets output */
    uint64_t crc_errors;  /* TS packets output with the error indicator set */
} drx_bbdec_stats;

drx_bbdec* drx_bbdec_new(void);
void drx_bbdec_free(drx_bbdec* d);

/*
 * Process one descrambled BBFRAME (packed, kbch_bytes long).
 *
 * TS packets recovered from it are written to ts_out, which must hold at
 * least kbch_bytes + DRX_TS_PACKET_LEN bytes. Returns the number of bytes
 * written (a multiple of 188), or -1 if the header is invalid. *h receives
 * the header, so the caller can route the output by h->isi. Packets whose
 * CRC fails are still output, with the transport error indicator set.
 * Non-TS streams (GSE) are not reconstructed and return 0.
 */
int drx_bbdec_frame(drx_bbdec* d,
                    const uint8_t* frame,
                    int kbch_bytes,
                    uint8_t* ts_out,
                    drx_bbheader* h);

void drx_bbdec_get_stats(const drx_bbdec* d, drx_bbdec_stats* s);

#ifdef __cplusplus
}
#endif

#endif /* DVBRX_BB_H */
