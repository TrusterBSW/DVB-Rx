#!/usr/bin/env python3
"""Wideband test capture: several channels side by side, as an SDR set to a
wide sample rate would see them (for the multichannel receiver).

usage: wideband.py <out.fc32> --rate HZ --seconds S [--snr DB]
                   FILE:RATE:OFFSET_HZ[:GAIN_DB] ...

Each FILE (fc32 at RATE) is resampled to --rate, moved to OFFSET_HZ and
added, at unit power plus GAIN_DB; --snr adds white noise over the whole
band relative to the strongest channel.
"""
import argparse
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly

p = argparse.ArgumentParser()
p.add_argument("out")
p.add_argument("channels", nargs="+")
p.add_argument("--rate", type=float, required=True)
p.add_argument("--seconds", type=float, required=True)
p.add_argument("--snr", type=float, default=None)
a = p.parse_args()

n = int(a.rate * a.seconds)
y = np.zeros(n, np.complex64)
gmax = -1e9
for spec in a.channels:
    f, rate, off, *g = spec.split(":")
    rate, off, g = float(rate), float(off), float(g[0]) if g else 0.0
    gmax = max(gmax, g)
    ratio = Fraction(a.rate / rate).limit_denominator(1000)
    need = int(n / float(ratio)) + 1000
    x = np.fromfile(f, np.complex64, count=need)
    x /= np.sqrt(np.mean(np.abs(x) ** 2))
    x = resample_poly(x, ratio.numerator, ratio.denominator).astype(np.complex64)[:n]
    x *= np.float32(10 ** (g / 20))
    x *= np.exp(2j * np.pi * off / a.rate * np.arange(len(x))).astype(np.complex64)
    y[: len(x)] += x
    print(f"{f}: {rate / 1e6:.3f} Msps x {ratio}, {off / 1e6:+.3f} MHz, {g:+.1f} dB")
if a.snr is not None:
    s = np.float32(10 ** ((gmax - a.snr) / 20) / np.sqrt(2))
    y += s * (np.random.randn(n).astype(np.float32) + 1j * np.random.randn(n).astype(np.float32))
y.tofile(a.out)
print(f"{a.out}: {n} samples at {a.rate / 1e6:.3f} Msps")
