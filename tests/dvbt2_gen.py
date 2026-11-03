#!/usr/bin/env python3
"""DVB-T2 (EN 302 755) test signal from gr-dtv's transmitter blocks.

usage: dvbt2_gen.py <in.ts> <out.fc32> [--fft 8k] [--guard 1/8] [--pp 7]
                    [--qam 64] [--rate 2/3] [--short] [--norot] [--ext]
                    [--l1 16] [--symbols N] [--frames 2]

out.fc32 is the baseband signal at the elementary rate (64/7 MHz for an
8 MHz channel), about unit power, a single PLP carrying in.ts.
"""
import argparse
import math

import numpy as np
from gnuradio import blocks, digital, dtv, gr

p = argparse.ArgumentParser()
p.add_argument("ts")
p.add_argument("out")
p.add_argument("--fft", default="8k", choices=["1k", "2k", "4k", "8k", "16k", "32k"])
p.add_argument("--guard", default="1/8", choices=["1/128", "1/32", "1/16", "19/256", "1/8", "19/128", "1/4"])
p.add_argument("--pp", type=int, default=7)
p.add_argument("--qam", type=int, default=64, choices=[4, 16, 64, 256])
p.add_argument("--rate", default="2/3", choices=["1/2", "3/5", "2/3", "3/4", "4/5", "5/6"])
p.add_argument("--short", action="store_true")
p.add_argument("--norot", action="store_true")
p.add_argument("--ext", action="store_true")
p.add_argument("--l1", type=int, default=16, choices=[2, 4, 16, 64])
p.add_argument("--symbols", type=int, default=0, help="data symbols per frame (0: about 100 ms)")
p.add_argument("--frames", type=int, default=2, help="T2-frames per super-frame")
p.add_argument("--ti", default="auto", help="TI blocks per frame: auto, or 'fec' for one FEC block each "
               "(gr-dtv's cell interleaver shift is off for later FEC blocks of a TI-block)")
a = p.parse_args()

fft_n = {"1k": 1024, "2k": 2048, "4k": 4096, "8k": 8192, "16k": 16384, "32k": 32768}[a.fft]
gnum, gden = (int(v) for v in a.guard.split("/"))
gi = {"1/128": dtv.GI_1_128, "1/32": dtv.GI_1_32, "1/16": dtv.GI_1_16, "19/256": dtv.GI_19_256,
      "1/8": dtv.GI_1_8, "19/128": dtv.GI_19_128, "1/4": dtv.GI_1_4}[a.guard]
t2gi = a.guard in ("1/128", "19/256", "19/128")
# gr-dtv's FFTSIZE_16K_T2GI (11) signals 1K in P1 (S2 = value & 7): 16K
# takes any guard interval under FFTSIZE_16K
fftsize = getattr(dtv, f"FFTSIZE_{a.fft.upper()}" + ("_T2GI" if t2gi and a.fft in ("8k", "32k") else ""))
framesize = dtv.FECFRAME_SHORT if a.short else dtv.FECFRAME_NORMAL
nldpc = 16200 if a.short else 64800
rate = getattr(dtv, "C" + a.rate.replace("/", "_"))
const = {4: dtv.MOD_QPSK, 16: dtv.MOD_16QAM, 64: dtv.MOD_64QAM, 256: dtv.MOD_256QAM}[a.qam]
bits = int(math.log2(a.qam))
ncells = nldpc // bits
rot = dtv.ROTATION_OFF if a.norot else dtv.ROTATION_ON
carriers = dtv.CARRIERS_EXTENDED if a.ext else dtv.CARRIERS_NORMAL
pp = getattr(dtv, f"PILOT_PP{a.pp}")
l1 = {2: dtv.L1_MOD_BPSK, 4: dtv.L1_MOD_QPSK, 16: dtv.L1_MOD_16QAM, 64: dtv.L1_MOD_64QAM}[a.l1]

# frame capacity (EN 302 755 tables 47-50; data cells counted roughly)
np2 = {1024: 16, 2048: 8, 4096: 4, 8192: 2, 16384: 1, 32768: 1}[fft_n]
cp2 = {1024: 558, 2048: 1118, 4096: 2236, 8192: 4472, 16384: 8944, 32768: 22432}[fft_n]
ktot = {1024: 853, 2048: 1705, 4096: 3409, 8192: 6817, 16384: 13633, 32768: 27265}[fft_n]
dx = {1: 3, 2: 6, 3: 6, 4: 12, 5: 12, 6: 24, 7: 24, 8: 6}[a.pp]
dy = {1: 4, 2: 2, 3: 4, 4: 2, 5: 4, 6: 2, 7: 4, 8: 16}[a.pp]
cdata_est = int(ktot * (1 - 1.0 / (dx * dy)) * 0.97)
sym_len = fft_n * (1 + gnum / gden)
nsym = a.symbols or max(3, int(0.1 * 64e6 / 7 / sym_len))
fecblocks = max(1, (np2 * cp2 - 2400 + (nsym - 1) * cdata_est) // ncells)
tiblocks = max(1, math.ceil(fecblocks * ncells / (2 ** 19 + 2 ** 15)))
if a.ti == "fec":
    fecblocks = min(fecblocks, 255)
    tiblocks = fecblocks
print(f"{a.fft} GI {a.guard} PP{a.pp} {a.qam}-QAM {a.rate}{' 16K' if a.short else ''}: "
      f"{nsym} data symbols, {fecblocks} FEC blocks, {tiblocks} TI blocks")

tb = gr.top_block()
# loop over whole packets (the file may not be a multiple of 188 bytes)
ts = open(a.ts, "rb").read()
ts = ts[: len(ts) // 188 * 188]
aligned = a.out + ".ts.tmp"
open(aligned, "wb").write(ts)
src = blocks.file_source(1, aligned, True)
# enough TS for four T2-frames
num, den = (int(v) for v in a.rate.split("/"))
head = blocks.head(1, max(188 * 6000, int(4.2 * fecblocks * nldpc * num / den / 8)))
bbh = dtv.dvb_bbheader_bb(dtv.STANDARD_DVBT2, framesize, rate, dtv.RO_0_35, dtv.INPUTMODE_NORMAL,
                          dtv.INBAND_OFF, fecblocks, 4000000)
scr = dtv.dvb_bbscrambler_bb(dtv.STANDARD_DVBT2, framesize, rate)
bch = dtv.dvb_bch_bb(dtv.STANDARD_DVBT2, framesize, rate)
ldpc = dtv.dvb_ldpc_bb(dtv.STANDARD_DVBT2, framesize, rate, dtv.MOD_OTHER)
inter = dtv.dvbt2_interleaver_bb(framesize, rate, const)
mod = dtv.dvbt2_modulator_bc(framesize, const, rot)
cell = dtv.dvbt2_cellinterleaver_cc(framesize, const, fecblocks, tiblocks)
fmap = dtv.dvbt2_framemapper_cc(framesize, rate, const, rot, fecblocks, tiblocks, carriers, fftsize, gi, l1,
                                pp, a.frames, nsym, dtv.PAPR_OFF, dtv.VERSION_111, dtv.PREAMBLE_T2_SISO,
                                dtv.INPUTMODE_NORMAL, dtv.RESERVED_OFF, dtv.L1_SCRAMBLED_OFF, dtv.INBAND_OFF)
freq = dtv.dvbt2_freqinterleaver_cc(carriers, fftsize, pp, gi, nsym, dtv.PAPR_OFF, dtv.VERSION_111,
                                    dtv.PREAMBLE_T2_SISO)
pil = dtv.dvbt2_pilotgenerator_cc(carriers, fftsize, pp, gi, nsym, dtv.PAPR_OFF, dtv.VERSION_111,
                                  dtv.PREAMBLE_T2_SISO, dtv.MISO_TX1, dtv.EQUALIZATION_OFF,
                                  dtv.BANDWIDTH_8_0_MHZ, fft_n)
papr = dtv.dvbt2_paprtr_cc(carriers, fftsize, pp, gi, nsym, dtv.PAPR_OFF, dtv.VERSION_111, 3.0, 10, fft_n)
cpre = digital.ofdm_cyclic_prefixer(fft_n, fft_n + fft_n * gnum // gden, 0, "")
p1 = dtv.dvbt2_p1insertion_cc(carriers, fftsize, gi, nsym, dtv.PREAMBLE_T2_SISO, dtv.SHOWLEVELS_OFF, 3.3)
snk = blocks.vector_sink_c()
tb.connect(src, head, bbh, scr, bch, ldpc, inter, mod, cell, fmap, freq, pil, papr, cpre, p1, snk)
tb.run()

import os
os.remove(aligned)
x = np.array(snk.data(), np.complex64)
x /= np.sqrt(np.mean(np.abs(x) ** 2))
x.tofile(a.out)
print(f"{a.out}: {len(x)} samples")
