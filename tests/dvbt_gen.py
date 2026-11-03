#!/usr/bin/env python3
"""DVB-T (EN 300 744) test signal from gr-dtv's transmitter blocks.

usage: dvbt_gen.py <in.ts> <out.fc32> [--mode 2k|8k] [--qam 4|16|64]
                   [--rate 1/2|2/3|3/4|5/6|7/8] [--guard 1/32|1/16|1/8|1/4]

out.fc32 is the baseband signal at the OFDM elementary rate (64/7 MHz for
an 8 MHz channel), about unit power.
"""
import argparse

import numpy as np
from gnuradio import blocks, digital, dtv, gr

p = argparse.ArgumentParser()
p.add_argument("ts")
p.add_argument("out")
p.add_argument("--mode", default="2k", choices=["2k", "8k"])
p.add_argument("--qam", type=int, default=16, choices=[4, 16, 64])
p.add_argument("--rate", default="2/3", choices=["1/2", "2/3", "3/4", "5/6", "7/8"])
p.add_argument("--guard", default="1/32", choices=["1/32", "1/16", "1/8", "1/4"])
a = p.parse_args()

mode = dtv.T2k if a.mode == "2k" else dtv.T8k
fft_len = 2048 if a.mode == "2k" else 8192
nsize = 1512 if a.mode == "2k" else 6048
const = {4: dtv.MOD_QPSK, 16: dtv.MOD_16QAM, 64: dtv.MOD_64QAM}[a.qam]
rate = getattr(dtv, "C" + a.rate.replace("/", "_"))
guard = {"1/32": dtv.GI_1_32, "1/16": dtv.GI_1_16, "1/8": dtv.GI_1_8, "1/4": dtv.GI_1_4}[a.guard]
cp = fft_len // int(a.guard.split("/")[1])

tb = gr.top_block()
src = blocks.file_source(1, a.ts, False)
disp = dtv.dvbt_energy_dispersal(1)
rs = dtv.dvbt_reed_solomon_enc(2, 8, 0x11D, 255, 239, 8, 51, 8)
inter = dtv.dvbt_convolutional_interleaver(136, 12, 17)
coder = dtv.dvbt_inner_coder(1, nsize, const, dtv.NH, rate)
bitint = dtv.dvbt_bit_inner_interleaver(nsize, const, dtv.NH, mode)
symint = dtv.dvbt_symbol_inner_interleaver(nsize, mode, 1)
mapper = dtv.dvbt_map(nsize, const, dtv.NH, mode, 1)
ref = dtv.dvbt_reference_signals(gr.sizeof_gr_complex, nsize, fft_len, const, dtv.NH, rate, rate,
                                 guard, mode, 0, 0)
cpre = digital.ofdm_cyclic_prefixer(fft_len, fft_len + cp, 0, "")
snk = blocks.vector_sink_c()
tb.connect(src, disp, rs, inter, coder, bitint, symint, mapper, ref, cpre, snk)
tb.run()

x = np.array(snk.data(), np.complex64)
x /= np.sqrt(np.mean(np.abs(x) ** 2))
x.astype(np.complex64).tofile(a.out)
print(f"{a.out}: {len(x)} samples, {len(x) // (fft_len + cp)} OFDM symbols "
      f"({a.mode}, {a.qam}-QAM, {a.rate}, GI {a.guard})")
