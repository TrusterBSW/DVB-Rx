#!/usr/bin/env python3
"""DVB-C (EN 300 429) test signal: energy dispersal, RS(204,188) and the
convolutional interleaver (I=12, M=17) from gr-dtv's DVB-T blocks (the same
as DVB-C's), then byte to m-tuple conversion, differential coding of the two
MSBs and QAM mapping written from the standard (figures 5, 7 and 8, table 1).

usage: dvbc_gen.py <in.ts> <out.fc32> [--qam 64]

out.fc32 holds one sample per symbol, unit average energy (pulse-shape it
with txshape.py --rolloff 0.15).
"""
import argparse

import numpy as np
from gnuradio import blocks, dtv, gr

# Quadrant 1 of the cross constellations: (I, Q) -> q LSBs (figures 7, 8).
QAM32_Q1 = {
    (1, 1): 0b000, (3, 1): 0b001, (5, 1): 0b011,
    (1, 3): 0b100, (3, 3): 0b101, (5, 3): 0b111,
    (1, 5): 0b110, (3, 5): 0b010,
}
QAM128_ROWS = {  # Q: labels for I = 1, 3, 5, ...
    11: [0b11010, 0b11011, 0b01011, 0b01010],
    9: [0b11000, 0b11001, 0b01001, 0b01000],
    7: [0b10000, 0b10001, 0b10101, 0b10100, 0b11100, 0b11101],
    5: [0b10010, 0b10011, 0b10111, 0b10110, 0b11110, 0b11111],
    3: [0b00010, 0b00011, 0b00111, 0b00110, 0b01110, 0b01111],
    1: [0b00000, 0b00001, 0b00101, 0b00100, 0b01100, 0b01101],
}


def gray_to_bin(g):
    b = 0
    while g:
        b ^= g
        g >>= 1
    return b


def quadrant1(m):
    """q LSBs -> (I, Q) in quadrant 1, for 2^m-QAM."""
    q = m - 2
    if m == 5:
        return {v: p for p, v in QAM32_Q1.items()}
    if m == 7:
        return {v: (2 * i + 1, qv) for qv, row in QAM128_ROWS.items() for i, v in enumerate(row)}
    # square: Q level from b(q-1), b(q-3), ..., I level from b(q-2), ..., b0, Gray coded
    table = {}
    for v in range(1 << q):
        qg = ig = 0
        for k in range(q - 1, -1, -1):
            bit = (v >> k) & 1
            if (k - (q - 1)) % 2 == 0:
                qg = qg << 1 | bit
            else:
                ig = ig << 1 | bit
        table[v] = (2 * gray_to_bin(ig) + 1, 2 * gray_to_bin(qg) + 1)
    return table


# rotation applied to quadrant 1 for the quadrant labelled IkQk (table 1)
ROTATION = {0b00: 1, 0b10: 1j, 0b11: -1, 0b01: -1j}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ts")
    p.add_argument("out")
    p.add_argument("--qam", type=int, default=64, choices=[16, 32, 64, 128, 256])
    a = p.parse_args()
    m = {16: 4, 32: 5, 64: 6, 128: 7, 256: 8}[a.qam]

    tb = gr.top_block()
    src = blocks.file_source(1, a.ts, False)
    disp = dtv.dvbt_energy_dispersal(1)
    rs = dtv.dvbt_reed_solomon_enc(2, 8, 0x11D, 255, 239, 8, 51, 8)
    inter = dtv.dvbt_convolutional_interleaver(136, 12, 17)
    snk = blocks.vector_sink_b()
    tb.connect(src, disp, rs, inter, snk)
    tb.run()

    data = np.array(snk.data(), np.uint8)
    bits = np.unpackbits(data)  # MSB first: byte to m-tuple conversion
    bits = bits[: len(bits) // m * m].reshape(-1, m)
    weights = 1 << np.arange(m - 1, -1, -1)
    tuples = bits @ weights

    q1 = quadrant1(m)
    lsb_mask = (1 << (m - 2)) - 1
    pts = np.array([complex(*q1[v]) for v in range(lsb_mask + 1)])
    rot = np.array([ROTATION[k] for k in range(4)])
    prev_i = prev_q = 0
    sym = np.empty(len(tuples), np.complex128)
    for n, t in enumerate(tuples):
        a_k, b_k = (t >> (m - 1)) & 1, (t >> (m - 2)) & 1
        if a_k ^ b_k == 0:
            i_k, q_k = a_k ^ prev_i, b_k ^ prev_q
        else:
            i_k, q_k = a_k ^ prev_q, b_k ^ prev_i
        prev_i, prev_q = i_k, q_k
        sym[n] = pts[t & lsb_mask] * rot[i_k << 1 | q_k]
    sym /= np.sqrt(np.mean(np.abs(sym) ** 2))
    sym.astype(np.complex64).tofile(a.out)
    print(f"{a.out}: {len(sym)} {a.qam}-QAM symbols")


if __name__ == "__main__":
    main()
