#!/usr/bin/env python3
"""Generate const_tables.h: for every DVB-S2 and DVB-S2X MODCOD (VL-SNR
excepted), the PLS code, the constellation (point for each bit label) and the
bit interleaver's column read order, all measured on the gr-dtv transmitter.

usage: gen_const_tables.py <small.ts> <gr-dtv dvbs2_physical_cc_impl.cc> \\
                           <src/fec_table.h> > src/const_tables.h

For each candidate MODCOD, a few frames go through dtv's LDPC encoder,
interleaver, modulator and PL framer:
- the interleaver output gives each symbol's bit label and the modulator
  output its point;
- the column order comes from matching label bits with the LDPC codeword
  written column-wise;
- the PLS code is decoded from the PL header with the (64, 8) Reed-Muller
  code and the S2X pi/2-BPSK rotation, independently of gr-dtv's code.
Candidate (constellation, rate) pairs are the cases listed in gr-dtv's PL
framer; only those with LDPC/BCH parameters in fec_table.h are kept.
"""
import re
import sys

import numpy as np
from gnuradio import blocks, dtv, gr

BITS = {"QPSK": 2, "8PSK": 3, "8APSK": 3, "16APSK": 4, "8_8APSK": 4, "32APSK": 5,
        "4_12_16APSK": 5, "4_8_4_16APSK": 5, "64APSK": 6, "8_16_20_20APSK": 6,
        "4_12_20_28APSK": 6, "128APSK": 7, "256APSK": 8}

# gr-dtv accepts these, but DVB-S2X defines each PLS code for one frame size
# only: 8PSK 26/45 is a short-frame MODCOD, 64APSK 4/5 and 5/6 normal ones.
EXCLUDE = {("8PSK", "26_45", "normal"), ("8_16_20_20APSK", "4_5", "short"),
           ("8_16_20_20APSK", "5_6", "short")}

# ---- PLS code (EN 302 307-1 5.5.2.4, EN 302 307-2 5.5.2) ------------------
RM_ROWS = [0x90AC2DDD, 0x55555555, 0x33333333, 0x0F0F0F0F, 0x00FF00FF, 0x0000FFFF,
           0xFFFFFFFF]
SCRAMBLER = 0x719D83C953422DFA


def pls_symbols(code):
    temp = 0
    for r in range(7):
        if code & (0x80 >> r):
            temp ^= RM_ROWS[r]
    bits = []
    for m in range(32):
        b = (temp >> (31 - m)) & 1
        bits += [b, b ^ (code & 1)]
    out = []
    for j, b in enumerate(bits):
        b ^= (SCRAMBLER >> (63 - j)) & 1
        k = 26 + j
        base = (1 + 1j) / np.sqrt(2) if k % 2 == 0 else (-1 + 1j) / np.sqrt(2)
        s = base * (1 - 2 * b)
        out.append(s * 1j if code & 0x80 else s)  # S2X: rotated by 90 degrees
    return np.array(out)


PLS_REF = np.array([pls_symbols(c) for c in range(256)])


def decode_pls(header):
    return int(np.argmax(np.real(PLS_REF.conj() @ header[26:90])))


# ---- candidates ---------------------------------------------------------------
def candidates(physical_src, fec_header):
    src = open(physical_src).read()
    known = {"normal": set(), "short": set()}
    fec = open(fec_header).read()
    for size in known:
        block = fec[fec.index(f"fec_{size}[]"):]
        block = block[:block.index("};")]
        known[size] = set(re.findall(r'\{ "(\w+)"', block))
    out = []
    for m in re.finditer(r"if \(constellation == MOD_(\w+)\) \{(.*?)\n    \}\n", src, re.S):
        const = m.group(1)
        if const not in BITS:
            continue
        for rate in re.findall(r"case C(\w+):", m.group(2)):
            for size in ("normal", "short"):
                if rate in known[size]:
                    out.append((const, rate, size))
    return out


def capture(ts, const, rate, frame_size):
    fsize = dtv.FECFRAME_NORMAL if frame_size == "normal" else dtv.FECFRAME_SHORT
    crate = getattr(dtv, "C" + rate)
    cst = getattr(dtv, "MOD_" + const)
    std = dtv.STANDARD_DVBS2
    tb = gr.top_block()
    src = blocks.file_source(1, ts, False)
    head = blocks.head(1, 60000)
    bbh = dtv.dvb_bbheader_bb(std, fsize, crate, dtv.RO_0_20, dtv.INPUTMODE_NORMAL,
                              dtv.INBAND_OFF, 168, 4000000)
    scr = dtv.dvb_bbscrambler_bb(std, fsize, crate)
    bch = dtv.dvb_bch_bb(std, fsize, crate)
    ldpc = dtv.dvb_ldpc_bb(std, fsize, crate, cst)  # 128APSK: 6 padding bits
    inter = dtv.dvbs2_interleaver_bb(fsize, crate, cst)
    mod = dtv.dvbs2_modulator_bc(fsize, crate, cst, dtv.INTERPOLATION_OFF)
    phy = dtv.dvbs2_physical_cc(fsize, crate, cst, dtv.PILOTS_OFF, 0)
    down = blocks.keep_m_in_n(gr.sizeof_gr_complex, 1, 2, 0)
    s_ldpc, s_int = blocks.vector_sink_b(), blocks.vector_sink_b()
    s_mod, s_phy = blocks.vector_sink_c(), blocks.vector_sink_c()
    tb.connect(src, head, bbh, scr, bch, ldpc, inter, mod, phy, down, s_phy)
    tb.connect(ldpc, s_ldpc)
    tb.connect(inter, s_int)
    tb.connect(mod, s_mod)
    tb.run()
    return (np.array(s_ldpc.data(), np.uint8), np.array(s_int.data(), np.uint8),
            np.array(s_mod.data(), np.complex64), np.array(s_phy.data(), np.complex64))


def analyse(const, rate, frame_size, ts):
    nbits = BITS[const]
    n = 64800 if frame_size == "normal" else 16200
    cw, labels, syms, phy = capture(ts, const, rate, frame_size)
    # 128APSK: 64800 + 6 padding bits in 9258 symbols, then 12 dummy symbols
    n_cw = n + 6 if const == "128APSK" else n
    nsym = n_cw // nbits
    nsym_frame = 9270 if const == "128APSK" else nsym
    nfr = min(len(cw) // n_cw, len(labels) // nsym_frame, len(syms) // nsym_frame)
    if nfr < 1 or len(phy) < 90:
        return None
    n = n_cw
    keep = np.concatenate([np.arange(f * nsym_frame, f * nsym_frame + nsym) for f in range(nfr)])
    cw, labels, syms = cw[:nfr * n], labels[keep], syms[keep]

    points = np.zeros(1 << nbits, np.complex64)
    for v in range(1 << nbits):
        sel = syms[labels == v]
        if not len(sel) or not np.allclose(sel, sel[0], atol=1e-5):
            return None
        points[v] = sel[0]

    order = []
    for k in range(nbits):
        lb = (labels[:nsym] >> (nbits - 1 - k)) & 1
        for c in range(nbits):
            if np.array_equal(lb, cw[c * nsym:(c + 1) * nsym]):
                order.append(c)
                break
        else:
            if nbits == 2 and np.array_equal(lb, cw[k:2 * nsym:2]):
                order.append(-1)  # QPSK: no interleaver, bits taken in pairs
                continue
            return None

    pls = decode_pls(phy[:90])
    if np.max(np.abs(phy[26:90] - PLS_REF[pls])) > 1e-4:
        return None
    return points, order, pls & 0xFE  # pilot flag cleared


def main():
    ts, physical_src, fec_header = sys.argv[1:4]
    rows, skipped = [], []
    for const, rate, fs in candidates(physical_src, fec_header):
        if (const, rate, fs) in EXCLUDE or "VLSNR" in rate:
            continue  # VL-SNR frames have their own header and layout
        res = analyse(const, rate, fs, ts)
        if res is None or res[2] in (0, 2):  # dummy code: not a MODCOD here
            skipped.append(f"{const} {rate} {fs}")
            continue
        rows.append((const, rate, fs, res))
        print(f"{const:16s} {rate:10s} {fs:6s} pls 0x{res[2]:02x}", file=sys.stderr)
    if skipped:
        print("skipped: " + ", ".join(skipped), file=sys.stderr)
    codes = [r[3][2] for r in rows]
    dup = {c for c in codes if codes.count(c) > 1}
    if dup:
        sys.exit("duplicate PLS codes: " + ", ".join(f"0x{c:02x}" for c in sorted(dup)))

    print("/* Generated by gen/gen_const_tables.py from the gr-dtv DVB-S2/S2X")
    print(" * transmitter (PLS code, constellation, interleaver). */")
    print("/* { PLS code (pilot bit 0), constellation, rate, short frame, bits,")
    print(" *   column order (-1: none), points indexed by bit label (MSB first) } */")
    print("static const struct const_row const_rows[] = {")
    for const, rate, fs, (pts, order, pls) in rows:
        order = (order + [0] * 8)[:8]
        p = ", ".join(f"{{ {z.real:.9f}f, {z.imag:.9f}f }}" for z in pts)
        print(f'    {{ 0x{pls:02x}, "{const}", "{rate}", {int(fs == "short")}, {BITS[const]}, '
              f"{{ {', '.join(map(str, order))} }},\n      {{ {p} }} }},")
    print("};")


if __name__ == "__main__":
    main()
