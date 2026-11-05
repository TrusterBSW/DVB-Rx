#!/usr/bin/env python3
"""Independent DVB-T 8K check receiver (no dvbrx code): per-symbol MER over a capture, then for the
poor symbols the shape of the error in time (additive noise, or a phase / amplitude disturbance of the
signal itself).

usage: dvbt_check.py CAPTURE.fc32 libdvbrx/src/dvbt.c [chunk_s] [max_s]
(capture at 10.24 MS/s; the pilot tables are read from dvbt.c; shape_at([t, ...]) gives the
error shape of the poorest symbol near each time)
"""
import re
import sys

import numpy as np
from scipy.signal import resample_poly

path, csrc = sys.argv[1], sys.argv[2]
CHUNK = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
MAXS = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
src = open(csrc).read()


def arr(name):
    body = re.search(name + r"\[\d+\] = \{([^}]*)\}", src).group(1)
    return np.array([int(v) for v in body.replace("\n", " ").split(",") if v.strip()])


CP = arr("CP_8K")
TPS = arr("TPS_8K")
N, G = 8192, 256
L = N + G
KMAX, KC = 6816, 3408
FS = 10.24e6
FE = 64e6 / 7  # 10.24 * 25 / 28
K = np.arange(KMAX + 1)
iscp = np.zeros(KMAX + 1, bool)
iscp[CP] = True
istps = np.zeros(KMAX + 1, bool)
istps[TPS] = True
LEV = np.arange(-7, 8, 2)

x = np.memmap(path, np.complex64, "r")
total = len(x) / FS


def sync(y):
    """symbol start (CP start) and fractional CFO (cycles/sample) by folded CP correlation"""
    n = len(y) - N
    p = y[:n] * np.conj(y[N:N + n])
    c = np.convolve(p, np.ones(G), "valid")
    S = len(c) // L
    f = c[:S * L].reshape(S, L).sum(0)
    i = int(np.argmax(np.abs(f)))
    eps = -np.angle(f[i]) / (2 * np.pi * N)
    return i, eps


def demod_chunk(t0, dur):
    i0 = int(t0 * FS)
    seg = np.asarray(x[i0:i0 + int(dur * FS)]).astype(np.complex128)
    y = resample_poly(seg, 25, 28)
    st, eps = sync(y[:40 * L])
    n = np.arange(len(y))
    y = y * np.exp(-2j * np.pi * eps * n)
    st2, eps2 = sync(y[:40 * L])  # refine
    y = y * np.exp(-2j * np.pi * eps2 * n)
    st = st2 + G // 2  # FFT window from the middle of the guard
    S = (len(y) - st - N) // L
    idx = st + np.arange(S)[:, None] * L + np.arange(N)[None, :]
    Y = np.fft.fft(y[idx], axis=1) / np.sqrt(N)
    # integer CFO from the continual pilots of consecutive symbols
    best, boff = -1, 0
    for off in range(-60, 61):
        b = (CP - KC + off) % N
        v = np.abs(np.sum(Y[1:12, b] * np.conj(Y[:11, b])))
        if v > best:
            best, boff = v, off
    X = Y[:, (K - KC + boff) % N]
    # per-symbol common phase and timing slope, cumulated from symbol to symbol on the CPs
    A = np.zeros(S)
    B = np.zeros(S)
    kk = CP - KC
    for s in range(1, S):
        R = X[s, CP] * np.conj(X[s - 1, CP])
        a0 = np.angle(R.sum())
        ang = np.angle(R * np.exp(-1j * a0))
        bb, aa = np.polyfit(kk, ang, 1)
        A[s] = A[s - 1] + a0 + aa
        B[s] = B[s - 1] + bb
    D = X * np.exp(-1j * (A[:, None] + B[:, None] * (K - KC)[None, :]))
    # scattered pilot phase
    sp = np.array([np.argmax([np.mean(np.abs(D[s, 3 * p::12]) ** 2) for p in range(4)]) for s in range(S)])
    # pilot gains (channel x pilot value), averaged over the chunk
    Gk = np.zeros(KMAX + 1, complex)
    cnt = np.zeros(KMAX + 1)
    for s in range(S):
        ks = np.arange(3 * sp[s], KMAX + 1, 12)
        Gk[ks] += D[s, ks]
        cnt[ks] += 1
    Gk[CP] = D[:, CP].mean(0)
    cnt[CP] = 1
    Gk = np.where(cnt > 0, Gk / np.maximum(cnt, 1), 0)
    # channel on k = 3i, pilot sign by continuity, linear in between
    H3 = Gk[0::3] / (4 / 3)
    for i in range(1, len(H3)):
        if np.real(H3[i] * np.conj(H3[i - 1])) < 0:
            H3[i] = -H3[i]
    H = np.zeros(KMAX + 1, complex)
    H[0::3] = H3
    for r in (1, 2):
        H[r::3] = H3[:-1] * (1 - r / 3) + H3[1:] * (r / 3)
    pil = Gk / np.where(np.abs(H) > 0, H, 1)  # pilot values (CP / SP)
    out = []
    for s in range(S):
        spm = np.zeros(KMAX + 1, bool)
        spm[3 * sp[s]::12] = True
        data = ~(iscp | istps | spm)
        Z = D[s] / H * np.sqrt(42)
        dec = np.clip(2 * np.floor(Z.real / 2) + 1, -7, 7) + 1j * np.clip(2 * np.floor(Z.imag / 2) + 1, -7, 7)
        err = np.abs(Z[data] - dec[data]) ** 2
        mer = 10 * np.log10(42 / err.mean())
        out.append((s, mer))
    return dict(y=y, st=st, S=S, X=X, D=D, A=A, B=B, H=H, pil=pil, sp=sp, off=boff, out=out, t0=t0, eps=eps + eps2)


def error_shape(c, s):
    """time-domain error of symbol s within its FFT window: total, and its parts along the signal
    (amplitude) and across it (phase), smoothed"""
    sp = c["sp"][s]
    spm = np.zeros(KMAX + 1, bool)
    spm[3 * sp::12] = True
    D, H = c["D"][s], c["H"]
    Z = D / H
    ideal = np.zeros(KMAX + 1, complex)
    q = Z * np.sqrt(42)
    dec = (np.clip(2 * np.floor(q.real / 2) + 1, -7, 7) + 1j * np.clip(2 * np.floor(q.imag / 2) + 1, -7, 7)) / np.sqrt(42)
    data = ~(iscp | istps | spm)
    ideal[data] = dec[data]
    ideal[iscp | spm] = c["pil"][iscp | spm]
    ideal[istps] = np.sign(Z[istps].real)
    rot = np.exp(1j * (c["A"][s] + c["B"][s] * (K - KC)))
    Xh = H * ideal * rot
    Xr = c["X"][s]
    bins = (K - KC + c["off"]) % N
    fh = np.zeros(N, complex)
    fe = np.zeros(N, complex)
    fh[bins] = Xh
    fe[bins] = Xr - Xh
    yh = np.fft.ifft(fh) * np.sqrt(N)
    e = np.fft.ifft(fe) * np.sqrt(N)
    w = 128
    k = np.ones(w) / w
    pw = np.convolve(np.abs(e) ** 2, k, "same") / np.mean(np.abs(yh) ** 2)
    proj = e * np.conj(yh) / (np.abs(yh) ** 2 + 1e-12)
    # smoothed along / across components (a coherent amplitude / phase change survives smoothing,
    # additive white noise mostly does not)
    amp = np.convolve(proj.real * np.abs(yh) ** 2, k, "same") / np.convolve(np.abs(yh) ** 2, k, "same")
    phs = np.convolve(proj.imag * np.abs(yh) ** 2, k, "same") / np.convolve(np.abs(yh) ** 2, k, "same")
    return pw, amp, phs


if __name__ == "__main__":
    allm = []
    t = 0.3
    keep = {}
    while t + CHUNK < min(total, MAXS):
        c = demod_chunk(t, CHUNK)
        for s, m in c["out"]:
            allm.append((t + (c["st"] + s * L) / FE, m))
        meds = np.median([m for _, m in c["out"]])
        bad = [s for s, m in c["out"] if m < meds - 3]
        if bad:
            keep[t] = (c, bad, meds)
        t += CHUNK
    allm = np.array(allm)
    med = np.median(allm[:, 1])
    print("symboles %d, MER médiane %.2f dB, symboles < médiane-3 dB : %d" % (len(allm), med, np.sum(allm[:, 1] < med - 3)))
    print("instants des creux :", " ".join("%.4fs(%.1f)" % (a, b) for a, b in allm[allm[:, 1] < med - 3][:60]))
    np.save("/tmp/tcheck_mer.npy", allm)
    # error shape: poor symbols vs normal ones
    rows = []
    for t0, (c, bad, meds) in keep.items():
        good = [s for s, m in c["out"] if abs(m - meds) < 0.5][:3]
        for s in bad[:4] + good[:1]:
            pw, amp, phs = error_shape(c, s)
            m = dict(c["out"])[s]
            seg = pw.reshape(16, -1).mean(1)
            rows.append((m < meds - 3, t0 + (c["st"] + s * L) / FE, m, 10 * np.log10(seg / np.median(seg)), np.std(amp), np.std(phs)))
    for poor, tt, m, seg, sa, sph in rows[:40]:
        print("%s t=%.4f MER %.1f | bruit par 1/16 de symbole (dB / médiane) : %s | écart-type lissé amplitude %.4f phase %.4f"
              % ("CREUX " if poor else "normal", tt, m, " ".join("%+.0f" % v for v in seg), sa, sph))


def shape_at(times):
    for tt in times:
        c = demod_chunk(tt - 0.1, 0.25)
        ts = np.array([c["t0"] + (c["st"] + s * L) / FE for s, _ in c["out"]])
        mers = np.array([m for _, m in c["out"]])
        med = np.median(mers)
        i = int(np.argmin(np.abs(ts - tt)))
        cand = [j for j in range(max(0, i - 4), min(len(ts), i + 5))]
        worst = min(cand, key=lambda j: mers[j])
        good = [j for j in range(len(ts)) if abs(mers[j] - med) < 0.2][:2]
        for j, lab in [(worst, "CREUX "), (good[0], "normal")]:
            pw, amp, phs = error_shape(c, j)
            seg = pw.reshape(32, -1).mean(1)
            print("%s t=%.4f MER %.1f | bruit par 1/32 de symbole (dB / médiane du normal) : %s" % (lab, ts[j], mers[j], " ".join("%+.0f" % (10 * np.log10(v)) for v in seg / np.median(error_shape(c, good[0])[0]))))
            print("        écart-type lissé (128 éch.) : amplitude %.4f, phase %.4f rad ; puissance d'erreur moyenne %.4f" % (np.std(amp), np.std(phs), pw.mean()))
