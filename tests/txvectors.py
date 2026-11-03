#!/usr/bin/env python3
"""Reference vectors from the gr-dtv DVB-S2 transmitter, one file per stage.

usage: txvectors.py <in.ts> <outdir> [-m MODCOD] [--frame-size normal|short]
                    [--pilots]

Files (one record per frame, raw bytes as produced by the gr-dtv blocks):
  bbframe.u8    BBHEADER + data field, unpacked bits (1 bit per byte)
  bbscram.u8    after BB scrambling, unpacked bits
  bch.u8        BCH codeword, unpacked bits
  ldpc.u8       LDPC codeword (FECFRAME), unpacked bits
  interleaved.u8  after the bit interleaver (one constellation symbol's bits
                  per byte)
  xfecframe.fc32  constellation symbols
  plframe.fc32    PL frames (header, scrambled payload, pilots), 1 sample/symbol
  info.txt      parameters and per-stage record sizes
"""
import argparse
import os
import re

from gnuradio import blocks, dtv, gr


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ts")
    p.add_argument("outdir")
    p.add_argument("-m", "--modcod", default="QPSK3/4")
    p.add_argument("--frame-size", default="normal", choices=["normal", "short"])
    p.add_argument("--pilots", action="store_true")
    p.add_argument("--rolloff", type=float, default=0.2)
    a = p.parse_args()

    # "QPSK3/4", "8_16_20_20APSK4/5", "64APSK128/180", ...
    constellation, code_rate = re.match(r"^(.*PSK)(\d+/\d+)$", a.modcod).groups()
    std = dtv.STANDARD_DVBS2
    fsize = dtv.FECFRAME_NORMAL if a.frame_size == "normal" else dtv.FECFRAME_SHORT
    crate = getattr(dtv, "C" + code_rate.replace("/", "_"))
    const = getattr(dtv, "MOD_" + constellation)
    rolloff = {0.35: dtv.RO_0_35, 0.25: dtv.RO_0_25, 0.2: dtv.RO_0_20}[a.rolloff]
    pilots = dtv.PILOTS_ON if a.pilots else dtv.PILOTS_OFF

    os.makedirs(a.outdir, exist_ok=True)
    tb = gr.top_block()
    src = blocks.file_source(1, a.ts, False)
    bbheader = dtv.dvb_bbheader_bb(std, fsize, crate, rolloff, dtv.INPUTMODE_NORMAL,
                                   dtv.INBAND_OFF, 168, 4000000)
    bbscram = dtv.dvb_bbscrambler_bb(std, fsize, crate)
    bch = dtv.dvb_bch_bb(std, fsize, crate)
    ldpc = dtv.dvb_ldpc_bb(std, fsize, crate, const)  # 128APSK: 6 padding bits
    inter = dtv.dvbs2_interleaver_bb(fsize, crate, const)
    mod = dtv.dvbs2_modulator_bc(fsize, crate, const, dtv.INTERPOLATION_OFF)
    phy = dtv.dvbs2_physical_cc(fsize, crate, const, pilots, 0)
    down = blocks.keep_m_in_n(gr.sizeof_gr_complex, 1, 2, 0)  # drop the zero stuffing
    chain = [src, bbheader, bbscram, bch, ldpc, inter, mod, phy, down]
    tb.connect(*chain)

    taps = [("bbframe.u8", bbheader, 1), ("bbscram.u8", bbscram, 1),
            ("bch.u8", bch, 1), ("ldpc.u8", ldpc, 1),
            ("interleaved.u8", inter, 1), ("xfecframe.fc32", mod, 8),
            ("plframe.fc32", down, 8)]
    sinks = []
    for name, blk, size in taps:
        snk = blocks.file_sink(size, os.path.join(a.outdir, name))
        snk.set_unbuffered(False)
        tb.connect(blk, snk)
        sinks.append((name, size))
    tb.run()

    nbbframes = os.path.getsize(os.path.join(a.outdir, "bbframe.u8"))
    with open(os.path.join(a.outdir, "info.txt"), "w") as f:
        f.write(f"modcod {a.modcod}\nframe_size {a.frame_size}\n"
                f"pilots {int(a.pilots)}\nrolloff {a.rolloff}\n")
        for name, size in sinks:
            f.write(f"{name} {os.path.getsize(os.path.join(a.outdir, name)) // size}\n")
    print(open(os.path.join(a.outdir, "info.txt")).read())


if __name__ == "__main__":
    main()
