#!/usr/bin/env python3
"""MER of a QPSK-like symbol file (fc32): removes the carrier offset found
from the spectrum of s^4, then a per-block residual phase.

usage: mer.py <symbols.fc32> [skip_symbols]
"""
import sys
import numpy as np

s = np.fromfile(sys.argv[1], dtype=np.complex64)
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 200_000
s = s[skip:skip + 2_000_000].astype(np.complex128)
# carrier frequency: peak of the spectrum of s^4 (zero-padded, then refined)
z = s ** 4
nfft = 1 << 22
spec = np.abs(np.fft.fft(z[:nfft // 4], nfft))
f = np.fft.fftfreq(nfft)[np.argmax(spec)] * 2 * np.pi / 4  # rad/symbol
d = s * np.exp(-1j * f * np.arange(len(s)))
# residual phase per short block
B = 256
d = d[:len(d) // B * B].reshape(-1, B)
ph = np.angle(-np.sum(d ** 4, axis=1)) / 4
d *= np.exp(-1j * ph)[:, None]
d = d.ravel()
ref = (np.sign(d.real) + 1j * np.sign(d.imag)) / np.sqrt(2) * np.sqrt(np.mean(np.abs(d) ** 2))
errs, pows = np.abs(d - ref) ** 2, np.abs(ref) ** 2
mer = 10 * np.log10(np.mean(pows) / np.mean(errs))
print(f"MER {mer:.2f} dB over {d.size} symbols, mean |s|^2 {np.mean(np.abs(s)**2):.3f}")
