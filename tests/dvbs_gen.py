#!/usr/bin/env python3
"""DVB-S (EN 300 421) test signal from gr-dtv's DVB-T channel coding blocks,
whose outer and inner coding are those of DVB-S: energy dispersal,
RS(204,188), convolutional interleaver (I=12, M=17), K=7 convolutional code
with puncturing, then QPSK.

usage: dvbs_gen.py <in.ts> <out.fc32> [--rate 3/4]

out.fc32 holds one sample per symbol (pulse-shape with txshape.py). The
QPSK bit to axis mapping is one of the 8 symmetric choices; the receiver
resolves the ambiguity anyway.
"""
import argparse

import numpy as np
from gnuradio import blocks, dtv, gr

p = argparse.ArgumentParser()
p.add_argument("ts")
p.add_argument("out")
p.add_argument("--rate", default="3/4", choices=["1/2", "2/3", "3/4", "5/6", "7/8"])
a = p.parse_args()

rate = getattr(dtv, "C" + a.rate.replace("/", "_"))
tb = gr.top_block()
src = blocks.file_source(1, a.ts, False)
disp = dtv.dvbt_energy_dispersal(1)  # one PRBS period: 8 packets = 1504 bytes
rs = dtv.dvbt_reed_solomon_enc(2, 8, 0x11D, 255, 239, 8, 51, 8)
inter = dtv.dvbt_convolutional_interleaver(136, 12, 17)
NOUT = 6048  # QPSK symbols per output vector, a whole number of bytes for every rate
coder = dtv.dvbt_inner_coder(1, NOUT, dtv.MOD_QPSK, dtv.NH, rate)
v2s = blocks.vector_to_stream(1, NOUT)
snk = blocks.vector_sink_b()
tb.connect(src, disp, rs, inter, coder, v2s, snk)
tb.run()

bits = np.array(snk.data(), np.uint8)  # two coded bits per symbol: (I, Q)
i = 1.0 - 2.0 * ((bits >> 1) & 1)
q = 1.0 - 2.0 * (bits & 1)
sym = ((i + 1j * q) / np.sqrt(2)).astype(np.complex64)
sym.tofile(a.out)
print(f"{a.out}: {len(sym)} QPSK symbols, rate {a.rate}")
