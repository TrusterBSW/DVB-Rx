/*
 * DVB-S2 baseband layer, see bb.h.
 */
#include "dvbrx/bb.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KBCH_BYTES (58192 / 8) /* normal frame, rate 9/10 */
#define TEI 0x80                    /* transport error indicator, TS byte 1 */

/* crc8_table[s][v]: CRC register after byte v then s zero bytes, for
 * slicing by 8 (eight independent lookups per 8 bytes instead of a chain
 * of dependent ones). */
static uint8_t crc8_table[8][256];
static uint8_t prbs[MAX_KBCH_BYTES];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; b++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0xD5) : (uint8_t)(c << 1);
        crc8_table[0][i] = c;
    }
    for (int t = 1; t < 8; t++)
        for (int i = 0; i < 256; i++)
            crc8_table[t][i] = crc8_table[0][crc8_table[t - 1][i]];

    /* BB scrambler: 1 + x^14 + x^15, initial state 100101010000000
     * (Section 5.2.2). The sequence restarts on every BBFRAME. */
    unsigned reg = 0x4A80;
    for (int i = 0; i < MAX_KBCH_BYTES; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            unsigned out = ((reg >> 1) ^ reg) & 1;
            reg = (reg >> 1) | (out << 14);
            byte = (uint8_t)((byte << 1) | out);
        }
        prbs[i] = byte;
    }
}

uint8_t drx_crc8(const uint8_t* data, int len)
{
    pthread_once(&tables_once, init_tables);
    uint8_t c = 0;
    int i = 0;
    for (; i + 8 <= len; i += 8) {
        const uint8_t* d = data + i;
        c = crc8_table[7][c ^ d[0]] ^ crc8_table[6][d[1]] ^ crc8_table[5][d[2]] ^
            crc8_table[4][d[3]] ^ crc8_table[3][d[4]] ^ crc8_table[2][d[5]] ^
            crc8_table[1][d[6]] ^ crc8_table[0][d[7]];
    }
    for (; i < len; i++)
        c = crc8_table[0][c ^ data[i]];
    return c;
}

void drx_bb_descramble(uint8_t* frame, int kbch_bytes)
{
    pthread_once(&tables_once, init_tables);
    for (int i = 0; i < kbch_bytes; i++)
        frame[i] ^= prbs[i];
}

int drx_bbheader_parse(const uint8_t* in, drx_bbheader* h)
{
    /* Normal mode: the CRC-8 of the 9 first bytes; DVB-T2 High Efficiency
     * Mode: the same XOR 1 (taken only for a TS without ISSY, whose UPL and
     * SYNC fields are then 0, so that a damaged DVB-S2 header is not read
     * as HEM). */
    const uint8_t c = drx_crc8(in, DRX_BBHEADER_LEN - 1);
    h->hem = 0;
    if (c != in[DRX_BBHEADER_LEN - 1]) {
        if ((c ^ 1) != in[DRX_BBHEADER_LEN - 1] || in[0] >> 6 != DRX_TS || (in[0] & 0x08) || in[2] || in[3] ||
            in[6])
            return -1;
        h->hem = 1;
    }
    h->ts_gs = in[0] >> 6;
    h->sis = (in[0] >> 5) & 1;
    h->ccm = (in[0] >> 4) & 1;
    h->issyi = (in[0] >> 3) & 1;
    h->npd = (in[0] >> 2) & 1;
    h->ro = in[0] & 3;
    h->isi = h->sis ? 0 : in[1];
    h->upl = (uint16_t)(in[2] << 8 | in[3]);
    h->dfl = (uint16_t)(in[4] << 8 | in[5]);
    h->sync = in[6];
    h->syncd = (uint16_t)(in[7] << 8 | in[8]);
    if (h->dfl % 8 || h->dfl > MAX_KBCH_BYTES * 8 - DRX_BBHEADER_LEN * 8)
        return -1;
    if (h->ts_gs == DRX_TS && (h->syncd > h->dfl || h->syncd % 8))
        return -1;
    return 0;
}

/* Per-ISI TS reconstruction state. */
struct stream {
    int synced;
    int partial; /* bytes of the current packet already collected */
    uint8_t pkt[DRX_TS_PACKET_LEN];
};

struct drx_bbdec {
    struct stream* streams[256];
    drx_bbdec_stats stats;
};

drx_bbdec* drx_bbdec_new(void)
{
    pthread_once(&tables_once, init_tables);
    return calloc(1, sizeof(drx_bbdec));
}

void drx_bbdec_free(drx_bbdec* d)
{
    if (!d)
        return;
    for (int i = 0; i < 256; i++)
        free(d->streams[i]);
    free(d);
}

void drx_bbdec_get_stats(const drx_bbdec* d, drx_bbdec_stats* s) { *s = d->stats; }

/* A user packet in the data field is 187 bytes (sync byte removed) followed
 * by the CRC-8 of those bytes, which takes the place of the next packet's
 * sync byte. Restore the sync byte and flag CRC failures. In HEM there is
 * no CRC: the 187 bytes only (the FEC is all the protection). */
static void emit(drx_bbdec* d, const uint8_t* up, uint8_t sync, int hem, uint8_t* out)
{
    out[0] = hem ? 0x47 : sync;
    memcpy(out + 1, up, DRX_TS_PACKET_LEN - 1);
    if (!hem && drx_crc8(up, DRX_TS_PACKET_LEN) != 0) {
        out[1] |= TEI;
        d->stats.crc_errors++;
    }
    d->stats.packets++;
}

int drx_bbdec_frame(drx_bbdec* d,
                    const uint8_t* frame,
                    int kbch_bytes,
                    uint8_t* out,
                    drx_bbheader* h)
{
    d->stats.frames++;
    if (drx_bbheader_parse(frame, h) || h->dfl / 8 > kbch_bytes - DRX_BBHEADER_LEN) {
        d->stats.bad_headers++;
        return -1;
    }
    if (h->ts_gs != DRX_TS || (!h->hem && h->upl != DRX_TS_PACKET_LEN * 8))
        return 0;
    /* bytes of a user packet in the data field, and the byte SYNCD points
     * at before it (normal mode: the CRC ending the previous packet) */
    const int L = h->hem ? DRX_TS_PACKET_LEN - 1 : DRX_TS_PACKET_LEN, lead = h->hem ? 0 : 1;

    struct stream* s = d->streams[h->isi];
    if (!s) {
        s = d->streams[h->isi] = calloc(1, sizeof(*s));
        if (!s)
            return -1;
    }

    const uint8_t* in = frame + DRX_BBHEADER_LEN;
    int left = h->dfl / 8;
    const int syncd = h->syncd / 8;
    int n = 0;

    /* SYNCD points at the CRC byte ending the packet in progress, so it must
     * match the bytes still missing from the partial packet. */
    if (s->synced && s->partial > 0 && syncd != L - lead - s->partial) {
        s->synced = 0;
        d->stats.gaps++;
    }
    if (!s->synced) {
        if (syncd + lead > left) /* no packet starts in this frame */
            return 0;
        in += syncd + lead;
        left -= syncd + lead;
        s->partial = 0;
        s->synced = 1;
    }

    if (s->partial > 0) {
        int need = L - s->partial;
        if (need > left) {
            memcpy(s->pkt + s->partial, in, left);
            s->partial += left;
            return 0;
        }
        memcpy(s->pkt + s->partial, in, need);
        emit(d, s->pkt, h->sync, h->hem, out);
        n += DRX_TS_PACKET_LEN;
        in += need;
        left -= need;
        s->partial = 0;
    }
    while (left >= L) {
        emit(d, in, h->sync, h->hem, out + n);
        n += DRX_TS_PACKET_LEN;
        in += L;
        left -= L;
    }
    memcpy(s->pkt, in, left);
    s->partial = left;
    return n;
}
