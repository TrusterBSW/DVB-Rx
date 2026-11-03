/*
 * BB layer test: reads scrambled BBFRAMEs (txvectors.py bbscram.u8, one bit
 * per byte), descrambles them, rebuilds the TS and writes it out.
 *
 * usage: test_bb <bbscram.u8> <kbch_bits> <out.ts>
 */
#include "dvbrx/bb.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <bbscram.u8> <kbch_bits> <out.ts>\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    FILE* o = fopen(argv[3], "wb");
    const int kbch = atoi(argv[2]), kbytes = kbch / 8;
    uint8_t* bits = malloc(kbch);
    uint8_t* frame = malloc(kbytes);
    uint8_t* ts = malloc(kbytes + DRX_TS_PACKET_LEN);
    drx_bbdec* d = drx_bbdec_new();
    if (!f || !o || !bits || !frame || !ts || !d)
        return 1;

    int nframes = 0;
    while (fread(bits, 1, kbch, f) == (size_t)kbch) {
        for (int i = 0; i < kbytes; i++) {
            uint8_t b = 0;
            for (int j = 0; j < 8; j++)
                b = (uint8_t)(b << 1 | (bits[8 * i + j] & 1));
            frame[i] = b;
        }
        drx_bb_descramble(frame, kbytes);
        drx_bbheader h;
        int n = drx_bbdec_frame(d, frame, kbytes, ts, &h);
        if (nframes++ == 0)
            printf("header: ts_gs=%d sis=%d ccm=%d npd=%d ro=%d isi=%d upl=%d dfl=%d "
                   "sync=0x%02x syncd=%d\n",
                   h.ts_gs, h.sis, h.ccm, h.npd, h.ro, h.isi, h.upl, h.dfl, h.sync,
                   h.syncd);
        if (n > 0)
            fwrite(ts, 1, n, o);
    }
    drx_bbdec_stats s;
    drx_bbdec_get_stats(d, &s);
    printf("frames %llu bad_headers %llu gaps %llu packets %llu crc_errors %llu\n",
           (unsigned long long)s.frames, (unsigned long long)s.bad_headers,
           (unsigned long long)s.gaps, (unsigned long long)s.packets,
           (unsigned long long)s.crc_errors);
    fclose(o);
    drx_bbdec_free(d);
    return s.bad_headers || s.crc_errors;
}
