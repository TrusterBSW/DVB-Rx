#!/usr/bin/env python3
"""Every DVB-S2X MODCOD of libdvbrx/src/const_tables.h through the gr-dtv
transmitter and the dvbrx receiver (PL sync -> TS), at a given margin above
the Shannon bound, with a carrier offset. Pilots on unless "nopilots" is given.

usage: run_s2x.py [margin_dB] [filter substring] [nopilots]
"""
import math
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "bench"))
from loopback import compare_ts  # noqa: E402

BITS = {"QPSK": 2, "8PSK": 3, "8APSK": 3, "16APSK": 4, "8_8APSK": 4, "4_12_16APSK": 5,
        "4_8_4_16APSK": 5, "64APSK": 6, "8_16_20_20APSK": 6, "4_12_20_28APSK": 6,
        "128APSK": 7, "256APSK": 8}

margin = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
filt = sys.argv[2] if len(sys.argv) > 2 else ""
pilots = not (len(sys.argv) > 3 and sys.argv[3] == "nopilots")
rows = re.findall(r'\{ 0x([0-9a-f]{2}), "(\w+)", "(\w+)", (\d),',
                  open(os.path.join(ROOT, "libdvbrx/src/const_tables.h")).read())
ts = os.path.join(ROOT, "work/ts300k.ts")
fails = 0
for pls, const, rate, short in rows:
    if int(pls, 16) < 0x80 or filt not in f"{const}{rate}":
        continue
    num, den = map(int, rate.split("_"))
    shannon = 10 * math.log10(2 ** (BITS[const] * num / den) - 1)
    snr = shannon + margin
    name = f"{const}{num}/{den}" + (" short" if short == "1" else "")
    d = os.path.join(ROOT, "work/vec/s2x", f"{const}_{rate}_{short}{'' if pilots else '_np'}")
    args = ["python3", os.path.join(ROOT, "tests/txvectors.py"), ts, d, "-m",
            f"{const}{num}/{den}"] + (["--pilots"] if pilots else [])
    if short == "1":
        args += ["--frame-size", "short"]
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    r = subprocess.run([os.path.join(ROOT, "libdvbrx/build/dvbrx_file"), "-n", f"{snr:.2f}",
                        "-f", "0.003", os.path.join(d, "plframe.fc32"),
                        os.path.join(d, "rx.ts")], capture_output=True, text=True)
    intact, n_out, n_ref = compare_ts(os.path.join(d, "in.ts") if os.path.exists(
        os.path.join(d, "in.ts")) else ts, os.path.join(d, "rx.ts"))
    m = re.search(r"SNR ([-0-9.]+) dB", r.stderr)
    it = re.search(r"avg ([0-9.]+) it", r.stderr)
    ok = n_out > 0 and intact == n_out and intact > 0.8 * n_ref
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} 0x{pls} {name:28s} EsN0 {snr:6.2f} dB "
          f"(Shannon {shannon:6.2f}) measured {m.group(1) if m else '?':>6s} dB "
          f"ldpc it {it.group(1) if it else '?':>4s}  {intact}/{n_ref} packets"
          f"{'' if intact == n_out else f' ({n_out - intact} bad)'}", flush=True)
sys.exit(1 if fails else 0)
