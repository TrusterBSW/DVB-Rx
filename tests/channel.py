#!/usr/bin/env python3
"""Channel impairments for OFDM tests: rate change with a sample clock
offset, carrier offset, a two-path echo and white noise.

usage: channel.py <in.fc32> <out.fc32> --in-rate HZ --out-rate HZ [--ppm P]
                  [--freq HZ] [--echo DELAY_S,GAIN_DB] [--snr DB]

The output rate is out-rate * (1 + ppm 1e-6): as if the receiver's clock
ran ppm slow. SNR is measured over the full output band.
"""
import argparse
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly

p = argparse.ArgumentParser()
p.add_argument("inp")
p.add_argument("out")
p.add_argument("--in-rate", type=float, required=True)
p.add_argument("--out-rate", type=float, required=True)
p.add_argument("--ppm", type=float, default=0)
p.add_argument("--freq", type=float, default=0)
p.add_argument("--echo", default=None)
p.add_argument("--snr", type=float, default=None)
a = p.parse_args()

x = np.fromfile(a.inp, np.complex64).astype(np.complex128)
x /= np.sqrt(np.mean(np.abs(x) ** 2))
if a.echo:
    delay, gain = (float(v) for v in a.echo.split(","))
    d = int(round(delay * a.in_rate))
    x[d:] += 10 ** (gain / 20) * np.exp(1j * 1.0) * x[:-d]
ratio = Fraction(a.out_rate * (1 + a.ppm * 1e-6) / a.in_rate).limit_denominator(40000)
y = resample_poly(x, ratio.numerator, ratio.denominator)
rate = a.in_rate * ratio
y *= np.exp(2j * np.pi * a.freq / rate * np.arange(len(y)))
if a.snr is not None:
    y /= np.sqrt(np.mean(np.abs(y) ** 2))
    s = 10 ** (-a.snr / 20) / np.sqrt(2)
    y += s * (np.random.randn(len(y)) + 1j * np.random.randn(len(y)))
y.astype(np.complex64).tofile(a.out)
print(f"{a.out}: {len(y)} samples at {float(rate):.3f} Hz (ratio {ratio})")
