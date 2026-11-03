#!/usr/bin/env python3
"""Multiple input stream (MIS) DVB-S2/S2X test signal generator.

Builds the BBFRAMEs of several transport streams (one ISI each, possibly
with different MODCODs, i.e. VCM) following EN 302 307-1 mode adaptation
(sync byte replaced by the CRC-8 of the previous packet, SYNCD, MIS
MATYPE), runs each stream through the gr-dtv chain from BB scrambling to PL
framing (with the given PL scrambling gold code), and interleaves the
PLFRAMEs of the streams frame by frame.

usage: mis_gen.py OUT.fc32 --stream ISI:TS:MODCOD [--stream ...] [--gold N]
                  [--pilots] [--frames N] [--pattern 0,0,1]

Writes OUT.fc32 (one sample per symbol) and, for each stream, the
transport stream actually carried: OUT.isi<ISI>.ts.
"""
import argparse
import os
import re

import numpy as np
from gnuradio import blocks, dtv, gr

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def crc8(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def kbch_of(rate, short):
    table = open(os.path.join(ROOT, "libdvbrx/src/fec_table.h")).read()
    block = table[table.index("fec_short[]" if short else "fec_normal[]"):]
    m = re.search(r'\{ "%s", (\d+),' % rate, block)
    return int(m.group(1))


def bbframes(ts, isi, kbch, nframes, ccm):
    """Yield nframes BBFRAMEs (bytes) carrying the packets of ts; also
    returns, through the last element, the TS bytes actually sent."""
    packets = [ts[i:i + 188] for i in range(0, len(ts) - 187, 188)]
    # Mode adaptation: each 188-byte UP keeps its 187 data bytes; its sync
    # byte position carries the CRC-8 of the previous UP's data bytes.
    stream = bytearray()
    starts = []
    prev_crc = 0
    sent = bytearray()
    k = 0
    dfl = kbch - 80
    frames = []
    while len(frames) < nframes:
        while len(stream) < dfl // 8 * (len(frames) + 2):
            p = packets[k % len(packets)]
            k += 1
            starts.append(len(stream))
            stream.append(prev_crc)
            stream += p[1:]
            prev_crc = crc8(p[1:])
            sent += p
        off = len(frames) * (dfl // 8)
        data = stream[off:off + dfl // 8]
        first = next(s for s in starts if s >= off)
        syncd = (first - off) * 8
        matype1 = 0xC0 | (0 << 5) | ((1 if ccm else 0) << 4) | 0x02  # TS, MIS, RO 0.20
        hdr = bytes([matype1, isi, 0x05, 0xE0, dfl >> 8, dfl & 0xFF, 0x47,
                     syncd >> 8, syncd & 0xFF])
        frames.append(hdr + bytes([crc8(hdr)]) + bytes(data))
    # TS bytes fully contained in the frames sent
    end = nframes * (dfl // 8)
    complete = sum(1 for s in starts if s + 188 <= end)
    return frames, bytes(sent[:complete * 188])


def modulate(frames, const, rate, short, pilots, gold):
    std = dtv.STANDARD_DVBS2
    fsize = dtv.FECFRAME_SHORT if short else dtv.FECFRAME_NORMAL
    crate = getattr(dtv, "C" + rate)
    cst = getattr(dtv, "MOD_" + const)
    bits = np.unpackbits(np.frombuffer(b"".join(frames), np.uint8))
    tb = gr.top_block()
    src = blocks.vector_source_b(bits.tolist(), False)
    scr = dtv.dvb_bbscrambler_bb(std, fsize, crate)
    bch = dtv.dvb_bch_bb(std, fsize, crate)
    ldpc = dtv.dvb_ldpc_bb(std, fsize, crate, cst)  # 128APSK: 6 padding bits
    inter = dtv.dvbs2_interleaver_bb(fsize, crate, cst)
    mod = dtv.dvbs2_modulator_bc(fsize, crate, cst, dtv.INTERPOLATION_OFF)
    phy = dtv.dvbs2_physical_cc(fsize, crate, cst,
                                dtv.PILOTS_ON if pilots else dtv.PILOTS_OFF, gold)
    down = blocks.keep_m_in_n(gr.sizeof_gr_complex, 1, 2, 0)
    snk = blocks.vector_sink_c()
    tb.connect(src, scr, bch, ldpc, inter, mod, phy, down, snk)
    tb.run()
    sym = np.array(snk.data(), np.complex64)
    return sym.reshape(len(frames), -1)  # one PLFRAME per row


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--stream", action="append", required=True,
                   help="ISI:TSFILE:MODCOD, e.g. 5:a.ts:8PSK3/4 (append 's' for short frames)")
    p.add_argument("--gold", type=int, default=0)
    p.add_argument("--pilots", action="store_true")
    p.add_argument("--frames", type=int, default=40, help="BBFRAMEs per stream")
    p.add_argument("--pattern", default="", help="stream index per frame slot, e.g. 0,0,1")
    a = p.parse_args()

    streams = []
    for spec in a.stream:
        isi, ts, mc = spec.split(":")
        short = mc.endswith("s")
        const, rate = re.match(r"^(.*PSK)(\d+/\d+)s?$", mc).groups()
        streams.append(dict(isi=int(isi), ts=open(ts, "rb").read(), const=const,
                            rate=rate.replace("/", "_"), short=short))
    ccm = len({(s["const"], s["rate"], s["short"]) for s in streams}) == 1
    for s in streams:
        kbch = kbch_of(s["rate"], s["short"])
        frames, sent = bbframes(s["ts"], s["isi"], kbch, a.frames, ccm)
        s["pl"] = modulate(frames, s["const"], s["rate"], s["short"], a.pilots, a.gold)
        with open(f"{a.out}.isi{s['isi']}.ts", "wb") as f:
            f.write(sent)

    pattern = [int(x) for x in a.pattern.split(",")] if a.pattern else list(range(len(streams)))
    pos = [0] * len(streams)
    out = []
    while any(pos[i] < len(s["pl"]) for i, s in enumerate(streams)):
        for i in pattern:
            if pos[i] < len(streams[i]["pl"]):
                out.append(streams[i]["pl"][pos[i]])
                pos[i] += 1
    np.concatenate(out).astype(np.complex64).tofile(a.out)
    print(f"{a.out}: {len(out)} PLFRAMEs, {'CCM' if ccm else 'VCM'}, gold {a.gold}, "
          + ", ".join(f"ISI {s['isi']} {s['const']} {s['rate']}{' short' if s['short'] else ''}"
                      for s in streams))


if __name__ == "__main__":
    main()
