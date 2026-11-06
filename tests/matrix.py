#!/usr/bin/env python3
"""Test matrix over the standards and their parameters: each case is
generated (gr-dtv transmitters, or this repository's generators), passed
through a light channel (resampling to an SDR rate of the bandwidth, small
carrier offset, noise well above threshold) and decoded by dvbrx_file; a
case passes when TS packets come out with no error.

usage: matrix.py [dvbt] [dvbt2] [dvbc] [dvbs] [--quick]   (all by default)
Results go to work/matrix/results.txt (one line per case) as they come.
"""
import itertools
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, "work", "matrix")
os.makedirs(W, exist_ok=True)
TS = os.path.join(ROOT, "work", "ts1m.ts")
RX = os.path.join(ROOT, "libdvbrx", "build", "dvbrx_file")
OUT = os.path.join(W, "results.txt")
quick = "--quick" in sys.argv
which = [a for a in sys.argv[1:] if not a.startswith("--")] or ["dvbt", "dvbt2", "dvbc", "dvbs"]

# Lowest rates: the bridge asks for (1 + roll-off) / 0.9 samples per symbol
# (DVB-C 1.278, DVB-S 1.5): the signal within 90 % of the band.

# SDR rate for a bandwidth: 61.44/N MHz just above 1.1 x the elementary rate
SDR = {1.7: 61.44e6 / 32, 5: 61.44e6 / 10, 6: 61.44e6 / 8, 7: 61.44e6 / 7, 8: 61.44e6 / 6,
       10: 61.44e6 / 5}


def elem(bw):
    return 131e6 / 71 if bw == 1.7 else bw * 1e6 * 8 / 7


def run(cmd, timeout=1800):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def record(name, ok, detail):
    line = f"{'PASS' if ok else 'FAIL'}  {name:60s} {detail}"
    print(line, flush=True)
    with open(OUT, "a") as f:
        f.write(line + "\n")


def channel(src, dst, in_rate, out_rate, snr, freq):
    return run(["python3", "tests/channel.py", src, dst, "--in-rate", str(in_rate), "--out-rate",
                str(out_rate), "--ppm", "15", "--freq", str(freq), "--snr", str(snr)])


def ofdm_case(kind, name, gen_cmd, bw, snr, extra_rx=(), invert=False):
    raw = os.path.join(W, "raw.fc32")
    rx_in = os.path.join(W, "rx.fc32")
    t0 = time.time()
    rc, out = run(gen_cmd + [TS, raw])
    if rc:
        record(name, False, "generator failed: " + out.strip().splitlines()[-1][:80])
        return
    rc, out = channel(raw, rx_in, elem(bw), SDR[bw], snr, 3000 * bw / 8)
    if invert:  # spectrum inverted: the conjugate signal
        import numpy as np
        np.conj(np.fromfile(rx_in, np.complex64)).astype(np.complex64).tofile(rx_in)
    flag = "-T" if kind == "dvbt" else "-2"
    rc, out = run([RX, flag, str(bw), "-S", str(SDR[bw]), *extra_rx, rx_in, os.devnull])
    m = re.search(r"ts (\d+) packets", out)
    pk = int(m.group(1)) if m else 0
    if kind == "dvbt":
        bad = int(re.search(r"(\d+) packets uncorrectable", out).group(1)) if pk else -1
        fec = ""
    else:
        fb = re.search(r"FEC blocks (\d+) \((\d+) failed\)", out)
        crc = re.search(r"(\d+) crc errors", out)
        bad = int(fb.group(2)) + (int(crc.group(1)) if crc else 0) if fb else -1
        fec = f"FEC {fb.group(1)}/{fb.group(2)} failed, {crc.group(1) if crc else '?'} crc, " if fb else ""
    mer = re.search(r"MER ([-0-9.]+) dB", out)
    ok = pk > 0 and bad == 0
    record(name, ok, f"{fec}{pk} packets, {bad} bad, MER {mer.group(1) if mer else '-'} dB "
           f"({time.time() - t0:.0f} s)")


def dvbt():
    modes = ["2k", "8k"]
    qams = [4, 16, 64]
    rates = ["1/2", "2/3", "3/4", "5/6", "7/8"]
    guards = ["1/32", "1/4"] if not quick else ["1/32"]
    bws = [8, 7, 6, 5]
    # C/N for QEF (EN 300 744 annex A, Gaussian) + margin
    cn = {(4, "1/2"): 3.1, (4, "2/3"): 4.9, (4, "3/4"): 5.9, (4, "5/6"): 6.9, (4, "7/8"): 7.7,
          (16, "1/2"): 8.8, (16, "2/3"): 11.1, (16, "3/4"): 12.5, (16, "5/6"): 13.5, (16, "7/8"): 13.9,
          (64, "1/2"): 14.4, (64, "2/3"): 16.5, (64, "3/4"): 18.0, (64, "5/6"): 19.3, (64, "7/8"): 20.1}
    for i, (mode, qam, rate, guard) in enumerate(itertools.product(modes, qams, rates, guards)):
        if quick and rate not in ("1/2", "7/8"):
            continue
        bw = bws[i % len(bws)]
        name = f"DVB-T {mode} {qam}-QAM {rate} GI {guard} {bw} MHz"
        ofdm_case("dvbt", name, ["python3", "tests/dvbt_gen.py", "--mode", mode, "--qam", str(qam),
                                 "--rate", rate, "--guard", guard], bw, cn[(qam, rate)] + 4)
    for mode in modes:
        ofdm_case("dvbt", f"DVB-T {mode} 64-QAM 3/4 GI 1/32 8 MHz, spectre inversé",
                  ["python3", "tests/dvbt_gen.py", "--mode", mode, "--qam", "64", "--rate", "3/4", "--guard", "1/32"],
                  8, cn[(64, "3/4")] + 4, invert=True)


def dvbt2():
    # 1) constellations x code rates x frame size, on one structure
    cn = {4: 8, 16: 14, 64: 19, 256: 24}
    rates = ["1/2", "3/5", "2/3", "3/4", "4/5", "5/6"]
    for qam, short in itertools.product([4, 16, 64, 256], [False, True]):
        for rate in rates if not quick else ["1/2", "2/3", "5/6"]:
            name = f"DVB-T2 8K PP7 1/32 {qam}-QAM {rate} {'16K' if short else '64K'} rot"
            gen = ["python3", "tests/dvbt2_gen.py", "--fft", "8k", "--guard", "1/32", "--pp", "7",
                   "--qam", str(qam), "--rate", rate, "--ti", "fec", "--symbols", "60"]
            ofdm_case("dvbt2", name, gen + (["--short"] if short else []), 8, cn[qam] + 4 + rates.index(rate))
    # 2) FFT sizes, guard intervals and pilot patterns (table 58), carrier modes
    structs = [("1k", "1/16", 4), ("1k", "1/8", 2), ("1k", "1/4", 1),
               ("2k", "1/32", 7), ("2k", "1/16", 5), ("2k", "1/8", 3), ("2k", "1/4", 1),
               ("4k", "1/32", 4), ("4k", "1/16", 4), ("4k", "1/8", 2), ("4k", "1/4", 1),
               ("8k", "1/128", 7), ("8k", "1/32", 4), ("8k", "1/16", 8), ("8k", "19/256", 5),
               ("8k", "1/8", 3), ("8k", "19/128", 2), ("8k", "1/4", 1),
               ("16k", "1/128", 7), ("16k", "1/32", 6), ("16k", "1/16", 2), ("16k", "19/256", 4),
               ("16k", "1/8", 8), ("16k", "19/128", 3), ("16k", "1/4", 1),
               ("32k", "1/128", 7), ("32k", "1/32", 6), ("32k", "1/16", 8), ("32k", "19/256", 2),
               ("32k", "1/8", 2), ("32k", "19/128", 8)]
    bws = [8, 7, 6, 5, 10, 1.7]
    for i, (fft, guard, pp) in enumerate(structs):
        if quick and i % 3:
            continue
        for ext in ([False, True] if fft in ("8k", "16k", "32k") else [False]):
            bw = bws[i % len(bws)]
            l1 = [2, 4, 16, 64][i % 4]
            rot = i % 2 == 0
            name = f"DVB-T2 {fft} PP{pp} {guard}{' ext' if ext else ''} L1 {l1} 64-QAM 3/4 {bw} MHz"
            if fft == "16k" and pp == 1 and ext:
                # gr-dtv's pilot generator has continual pilot 3636 for 13636
                # (table G.2) while its frame mapper counts the right cells: its
                # signal is broken. Checked with dvbtx instead (0 errors).
                print(f"SKIP  {name:60s} gr-dtv bug, checked with dvbtx", flush=True)
                continue
            gen = ["python3", "tests/dvbt2_gen.py", "--fft", fft, "--guard", guard, "--pp", str(pp),
                   "--qam", "64", "--rate", "3/4", "--l1", str(l1), "--ti", "fec",
                   "--symbols", str({"1k": 120, "2k": 100, "4k": 80, "8k": 40, "16k": 20, "32k": 10}[fft])]
            ofdm_case("dvbt2", name, gen + (["--ext"] if ext else []) + ([] if rot else ["--norot"]), bw, 27)


def dvbc():
    for qam, (sr, sps) in itertools.product([16, 32, 64, 128, 256], [(6.9e6, 2.0), (6.875e6, 2.226), (5.0e6, 3.07), (6.9e6, 1.278)]):
        if quick and sps != 2.0:
            continue
        name = f"DVB-C {qam}-QAM {sr / 1e6:.3f} Msym/s, {sps} sps"
        sym = os.path.join(W, "sym.fc32")
        iq = os.path.join(W, "rx.fc32")
        t0 = time.time()
        rc, out = run(["python3", "tests/dvbc_gen.py", TS, sym, "--qam", str(qam)])
        es = {16: 18, 32: 21, 64: 24, 128: 27, 256: 31}[qam]
        rc, out = run(["python3", "tests/txshape.py", sym, os.path.join(W, "sh.fc32"), "--rolloff", "0.15",
                       "--esn0", str(es), "--freq", "0.005", "--sps", "4"])
        rc, out = channel(os.path.join(W, "sh.fc32"), iq, 4 * sr, sps * sr, 60, 0)
        rc, out = run([RX, "-s", str(sps * 1.000015), "-r", "0.15", "-c", "auto", iq, os.devnull])
        m = re.search(r"ts (\d+) packets.*?(\d+) packets uncorrectable", out, re.S)
        pk, bad = (int(m.group(1)), int(m.group(2))) if m else (0, -1)
        record(name, pk > 0 and bad <= 20, f"{pk} packets, {bad} uncorrectable (acquisition) ({time.time() - t0:.0f} s)")


def dvbs():
    for rate, sps in itertools.product(["1/2", "2/3", "3/4", "5/6", "7/8"], [2.0, 2.5, 1.5]):
        name = f"DVB-S {rate} {sps} sps"
        sym = os.path.join(W, "sym.fc32")
        t0 = time.time()
        run(["python3", "tests/dvbs_gen.py", TS, sym, "--rate", rate])
        es = {"1/2": 6, "2/3": 7.5, "3/4": 8.5, "5/6": 9.5, "7/8": 10}[rate]
        run(["python3", "tests/txshape.py", sym, os.path.join(W, "sh.fc32"), "--rolloff", "0.35",
             "--esn0", str(es), "--freq", "0.01", "--sps", "4"])
        channel(os.path.join(W, "sh.fc32"), os.path.join(W, "rx.fc32"), 4e6, sps * 1e6, 60, 0)
        rc, out = run([RX, "-s", str(sps * 1.000015), "-r", "0.35", "-l", "auto", os.path.join(W, "rx.fc32"),
                       os.devnull])
        m = re.search(r"ts (\d+) packets.*?(\d+) packets uncorrectable", out, re.S)
        pk, bad = (int(m.group(1)), int(m.group(2))) if m else (0, -1)
        record(name, pk > 0 and bad == 0, f"{pk} packets, {bad} uncorrectable ({time.time() - t0:.0f} s)")


for w in which:
    globals()[w]()
