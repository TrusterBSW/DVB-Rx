/*
 * Outer layer of DVB-S, DVB-C and DVB-T (internal): packet sync on the
 * 0x47 / 0xB8 sync bytes, Forney deinterleaver (I = 12, M = 17),
 * RS(204, 188) and energy dispersal removal. Bits or bytes in, 188-byte TS
 * packets out.
 */
#ifndef DVBRX_OUTER_H
#define DVBRX_OUTER_H

#include <stdint.h>

#define OUTER_PKT 204
#define OUTER_TS 188
#define OUTER_SYNC_PKTS 8
#define OUTER_SYNC_BITS ((OUTER_SYNC_PKTS * OUTER_PKT + 1) * 8)
#define OUTER_I 12
#define OUTER_M 17
#define OUTER_RING 4096 /* > (OUTER_I - 1) * OUTER_M * OUTER_I, a power of 2 */

typedef void (*outer_cb)(void* ctx, const uint8_t* ts, int len);

struct outer {
    int active;       /* decoding; cleared when the sync is lost */
    _Atomic int lost; /* sync lost: the inner receiver must search again */
    long timeout_bits; /* give up when no sync is found within this */

    uint8_t* sbits;
    int nsbits;
    long bits_since_reset;
    _Atomic int synced;
    int invert, nbit;
    uint8_t byte;
    long q;
    int misses;

    uint8_t* ring; /* deinterleaver input, OUTER_RING bytes */
    uint8_t pkt[OUTER_PKT];
    int group;

    _Atomic uint64_t packets, rs_corrected, rs_failed;
    outer_cb cb;
    void* ctx;
};

int outer_init(struct outer* o);
void outer_free(struct outer* o);
/* Start over: look for the packet sync again. */
void outer_reset(struct outer* o);
/* n bits in, one per byte, MSB of each byte first. Before sync, the packet
 * start is searched at every bit offset; inverted bits (180-degree phase
 * ambiguity of DVB-S) are detected too. */
void outer_bits(struct outer* o, const uint8_t* bits, int n);

/* RS(204, 188) decoding in place: number of bytes fixed, or -1. */
int outer_rs_decode(uint8_t* c);

#endif
