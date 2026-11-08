#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Non-regression campaign: every case is generated once (pulse shaping, then a channel with
a sample rate change, a 15 ppm clock offset and a carrier offset) and decoded by two builds of
dvbrx_file, OLD and NEW; their packets, uncorrectable packets (or BCH failures) and SNR are
compared. Each case is also run at a lower Es/N0 than the matrix's, near the threshold, where a
small loss shows up as errors.

usage: regress.py OLD_DVBRX_FILE NEW_DVBRX_FILE SYMDIR VECDIR [dvbs] [dvbc] [dvbs2] [-j N]
  SYMDIR: dvbs_12.fc32 ... dvbs_78.fc32 (tests/dvbs_gen.py) and dvbc_16.fc32 ... dvbc_256.fc32
  (tests/dvbc_gen.py), one symbol per sample; VECDIR: the DVB-S2 PL frames
  (tests/txvectors.py: VECDIR/<name>/plframe.fc32). Needs numpy and scipy only, so it runs on a
  machine without GNU Radio once the symbols are there. Results go to stdout and to
  work/regress/results.txt. REGRESS_ONLY=REGEX keeps the cases whose name matches.
"""
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "work", "regress")

args = [a for a in sys.argv[1:] if not a.startswith("-")]
jobs = 3
if "-j" in sys.argv:
    jobs = int(sys.argv[sys.argv.index("-j") + 1])
    args.remove(sys.argv[sys.argv.index("-j") + 1])
if len(args) < 4:
    print(__doc__)
    sys.exit(2)
OLD, NEW, SYM, VEC = args[:4]
which = args[4:] or ["dvbs", "dvbc", "dvbs2"]


def cases():
    out = []
    if "dvbs" in which:
        es = {"12": 6, "23": 7.5, "34": 8.5, "56": 9.5, "78": 10}
        for r, e in es.items():
            for sps in (1.5, 2.0, 2.048, 2.5):
                for de in (0, -1.5):
                    out.append(("DVB-S %s/%s %.3g sps Es/N0 %.1f" % (r[0], r[1], sps, e + de),
                                os.path.join(SYM, "dvbs_%s.fc32" % r), 0.35, e + de, sps, 0.01,
                                ["-l", "auto", "-r", "0.35"]))
    if "dvbc" in which:
        es = {16: 18, 32: 21, 64: 24, 128: 27, 256: 31}
        for q, e in es.items():
            for sps in (1.278, 2.0, 2.226, 3.07):
                for de in (0, -1.5):
                    out.append(("DVB-C %d-QAM %.4g sps Es/N0 %.1f" % (q, sps, e + de),
                                os.path.join(SYM, "dvbc_%d.fc32" % q), 0.15, e + de, sps, 0.005,
                                ["-c", "auto", "-r", "0.15"]))
    if "dvbs2" in which:
        # threshold + 1 dB, and + 0 (the threshold itself: the BCH failure count compares)
        es = {"qpsk34": 5.0, "np_qpsk3_4": 5.0, "8psk3_4": 9.0, "np_8psk3_4": 9.0,
              "16apsk3_4": 11.2, "np_16apsk3_4": 11.2, "32apsk4_5": 14.6}
        for v, e in es.items():
            for ro in (0.20, 0.10, 0.05):
                for sps in (2.0, round((1 + ro) / 0.9 + 0.003, 3)):
                    for de in (0, -1.0):
                        out.append(("DVB-S2 %s ro %.2f %.3g sps Es/N0 %.1f" % (v, ro, sps, e + de),
                                    os.path.join(VEC, v, "plframe.fc32"), ro, e + de, sps, 0.002,
                                    ["-r", "%.2f" % ro]))
    only = os.environ.get("REGRESS_ONLY")  # a regular expression on the case names
    return [c for c in out if not only or re.search(only, c[0])]


def decode(binary, iq, sps, extra):
    r = subprocess.run([binary, "-s", "%.6f" % (sps * (1 + 15e-6))] + extra + [iq, os.devnull],
                       capture_output=True, text=True, timeout=1800)
    o = r.stdout + r.stderr
    snr = re.findall(r"SNR ([-0-9.]+) dB", o)
    pk = re.search(r"ts (\d+) packets", o)
    bad = re.search(r"(\d+) packets uncorrectable", o)
    bch = re.search(r"bch failed (\d+)", o)
    return (float(snr[-1]) if snr else float("nan"), int(pk.group(1)) if pk else 0,
            int(bad.group(1)) if bad else -1, int(bch.group(1)) if bch else -1)


def run(case):
    name, src, ro, es, sps, freq, extra = case
    with tempfile.TemporaryDirectory(dir=OUTDIR) as d:
        sh, rx = os.path.join(d, "sh.fc32"), os.path.join(d, "rx.fc32")
        subprocess.run(["python3", os.path.join(ROOT, "tests/txshape.py"), src, sh, "--rolloff",
                        str(ro), "--esn0", str(es), "--freq", str(freq), "--sps", "4"],
                       check=True, capture_output=True)
        subprocess.run(["python3", os.path.join(ROOT, "tests/channel.py"), sh, rx, "--in-rate", "4",
                        "--out-rate", str(sps), "--ppm", "15", "--snr", "60"],
                       check=True, capture_output=True)
        return name, decode(OLD, rx, sps, extra), decode(NEW, rx, sps, extra)


def errors(r):
    """uncorrectable packets (DVB-S/C) or BCH failures (DVB-S2)"""
    return r[2] if r[3] < 0 else r[3]


os.makedirs(OUTDIR, exist_ok=True)
res_path = os.path.join(OUTDIR, "results.txt")
worse = 0
with open(res_path, "w") as res, concurrent.futures.ThreadPoolExecutor(jobs) as ex:
    hdr = "%-46s | %-27s | %-27s | verdict" % ("case", "old: SNR packets errors", "new: SNR packets errors")
    print(hdr, flush=True)
    res.write(hdr + "\n")
    for name, o, n in ex.map(run, cases()):
        eo, en = errors(o), errors(n)
        # worse: fewer packets out, or more errors, beyond a small acquisition tolerance; more
        # packets out counts first (a build that locks where the other does not also has errors)
        more = n[1] > o[1] + max(20, o[1] // 100)
        less = n[1] < o[1] - max(20, o[1] // 100)
        bad = less or (not more and en > eo + max(5, eo // 10))
        better = not bad and (more or en < eo - max(5, eo // 10))
        v = "WORSE" if bad else "better" if better else "same"
        worse += bad
        line = "%-46s | %6.2f %6d %6d       | %6.2f %6d %6d       | %s" % (name, o[0], o[1], eo, n[0], n[1], en, v)
        print(line, flush=True)
        res.write(line + "\n")
print("cases worse with NEW: %d (results in %s)" % (worse, res_path))
sys.exit(1 if worse else 0)
