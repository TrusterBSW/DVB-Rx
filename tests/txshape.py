#!/usr/bin/env python3
"""Pulse-shape PL frames (txvectors.py plframe.fc32, one sample per symbol)
into IQ at SPS samples per symbol with an RRC filter of any roll-off (the
DVB-S2X 0.15 / 0.10 / 0.05 included), then add noise and a carrier offset.

usage: txshape.py <plframe.fc32> <out.fc32> --rolloff A [--sps 2] [--esn0 dB]
                  [--freq cycles_per_symbol] [--delay symbols] [--pn deg_per_symbol]
"""
import argparse

import numpy as np


def rrc(alpha, sps, delay):
    t = np.arange(-delay * sps, delay * sps + 1) / sps
    h = np.empty_like(t)
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(4 * alpha * x) - 1) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) +
                                          (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / \
                   (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


p = argparse.ArgumentParser()
p.add_argument("inp")
p.add_argument("out")
p.add_argument("--rolloff", type=float, required=True)
p.add_argument("--sps", type=int, default=2)
p.add_argument("--esn0", type=float)
p.add_argument("--freq", type=float, default=0.0)
p.add_argument("--delay", type=int, default=40, help="RRC half-length (low roll-offs need long filters)")
p.add_argument("--pn", type=float, default=0.0,
               help="phase noise: Wiener process, standard deviation per symbol in degrees")
a = p.parse_args()

sym = np.fromfile(a.inp, np.complex64).astype(np.complex128)
up = np.zeros(len(sym) * a.sps, np.complex128)
up[::a.sps] = sym
x = np.convolve(up, rrc(a.rolloff, a.sps, a.delay) * np.sqrt(a.sps), mode="same")
# unit-energy pulse times sqrt(sps): unit sample power, Es = sps (sample
# units), so N0 = sps * noise power per sample / Es
n = np.arange(len(x))
x *= np.exp(2j * np.pi * a.freq / a.sps * n)
if a.pn > 0:
    walk = np.cumsum(np.random.randn(len(x)) * np.radians(a.pn) / np.sqrt(a.sps))
    x *= np.exp(1j * walk)
if a.esn0 is not None:
    var = a.sps * 10 ** (-a.esn0 / 10)  # noise power per sample
    x += np.sqrt(var / 2) * (np.random.randn(len(x)) + 1j * np.random.randn(len(x)))
x.astype(np.complex64).tofile(a.out)
