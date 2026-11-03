#!/usr/bin/env python3
"""DVB-S2 software loopback bench: TS -> dvbs2-tx -> IQ file -> dvbs2-rx -> TS.

Checks that the receiver recovers the input transport stream and measures how
many symbols per second it processes when running as fast as possible, which
tells whether a given symbol rate can be received in real time on this host.

Run after sourcing env.sh, e.g.:
    bench/loopback.py -m 8PSK3/5 --snr 9 --target-rate 27.5e6
"""
import argparse
import json
import os
import resource
import subprocess
import sys
import time

TS_PKT = 188
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def gen_iq(args, ts_path, iq_path):
    cmd = [
        "dvbs2-tx", "--source", "file", "--in-file", ts_path,
        "--sink", "file", "--out-file", iq_path,
        "-m", args.modcod, "--frame-size", args.frame_size,
        "-r", str(args.rolloff), "-s", str(args.sym_rate), "-o", str(args.sps),
        "--gold-code", str(args.gold_code),
        "--snr", str(args.snr), "--freq-offset", str(args.freq_offset),
    ]
    if args.pilots:
        cmd.append("--pilots")
    run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def rx(args, iq_path, out_path, log_path):
    cmd = [
        args.rx_app, "--source", "file", "--in-file", iq_path,
        "--sink", "file", "--out-file", out_path,
        "-m", args.modcod, "--frame-size", args.frame_size,
        "-r", str(args.rolloff), "-s", str(args.sym_rate), "-o", str(args.sps),
        "--gold-code", str(args.gold_code),
        "-p", "on" if args.pilots else "off",
        "--ldpc-iterations", str(args.ldpc_iterations),
        "--log-all", "--log-period", "1",
    ] + args.rx_args.split()
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    with open(log_path, "w") as log:
        run(cmd, stdout=log, stderr=subprocess.STDOUT)
    wall = time.monotonic() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
    return wall, cpu


def last_stats(log_path):
    stats = None
    with open(log_path) as f:
        for line in f:
            i = line.find("{")
            if i >= 0 and '"lock"' in line:
                try:
                    stats = json.loads(line[i:])
                except json.JSONDecodeError:
                    pass
    return stats


def compare_ts(ref_path, out_path):
    """Return (intact output packets, total output packets, input packets).

    An output packet counts as intact when it equals the next not-yet-matched
    occurrence of that packet in the input, so packets corrupted during
    acquisition, or duplicated, are not counted.
    """
    ref = open(ref_path, "rb").read()
    out = open(out_path, "rb").read()
    n_ref = len(ref) // TS_PKT
    positions = {}
    for k in range(n_ref):
        positions.setdefault(ref[k * TS_PKT:(k + 1) * TS_PKT], []).append(k)
    last = -1
    intact = 0
    n_out = len(out) // TS_PKT
    for k in range(n_out):
        cand = positions.get(out[k * TS_PKT:(k + 1) * TS_PKT])
        if not cand:
            continue
        nxt = next((c for c in cand if c > last), None)
        if nxt is not None:
            intact += 1
            last = nxt
    return intact, n_out, n_ref


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-m", "--modcod", default="QPSK3/4")
    p.add_argument("--frame-size", choices=["normal", "short"], default="normal")
    p.add_argument("--pilots", action=argparse.BooleanOptionalAction, default=True)
    p.add_argument("-r", "--rolloff", type=float, default=0.2)
    p.add_argument("--sps", type=float, default=2.0,
                   help="samples per symbol of the generated IQ")
    p.add_argument("--sym-rate", type=float, default=1e6,
                   help="nominal symbol rate given to tx/rx (only scales the "
                        "frequency offset; processing cost is per symbol)")
    p.add_argument("--gold-code", type=int, default=0)
    p.add_argument("--snr", type=float, default=10.0, help="Es/N0 in dB")
    p.add_argument("--freq-offset", type=float, default=5e3, help="Hz")
    p.add_argument("--ldpc-iterations", type=int, default=25)
    p.add_argument("--ts", default=os.path.join(ROOT, "work", "test.ts"))
    p.add_argument("--ts-bytes", type=int, default=4_000_000,
                   help="amount of input TS to transmit")
    p.add_argument("--target-rate", type=float, default=27.5e6,
                   help="symbol rate to compare the measured throughput with")
    p.add_argument("--rx-app", default=os.path.join(ROOT, "apps", "dvbrx-dvbs2-rx"),
                   help="receiver application (default: dvbrx-dvbs2-rx; "
                        "use dvbs2-rx for the original gr-dvbs2rx chain)")
    p.add_argument("--rx-args", default="",
                   help="extra arguments passed verbatim to dvbs2-rx")
    p.add_argument("--workdir", default=os.path.join(ROOT, "work", "loopback"))
    p.add_argument("--keep", action="store_true", help="keep the IQ file")
    args = p.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    tag = "{}_{}_{}_snr{}".format(args.modcod.replace("/", ""), args.frame_size,
                                  "p" if args.pilots else "np", args.snr)
    ts_in = os.path.join(args.workdir, "in.ts")
    iq = os.path.join(args.workdir, tag + ".fc32")
    ts_out = os.path.join(args.workdir, tag + ".ts")
    log = os.path.join(args.workdir, tag + ".log")

    with open(args.ts, "rb") as f:
        data = f.read(args.ts_bytes - args.ts_bytes % TS_PKT)
    with open(ts_in, "wb") as f:
        f.write(data)

    gen_iq(args, ts_in, iq)
    n_symbols = os.path.getsize(iq) / 8 / args.sps  # fc32 = 8 bytes/sample
    wall, cpu = rx(args, iq, ts_out, log)
    if not args.keep:
        os.remove(iq)

    intact, n_out, n_ref = compare_ts(ts_in, ts_out)
    stats = last_stats(log) or {}
    fec = stats.get("fec", {})
    rate = n_symbols / wall
    print(f"{os.path.basename(args.rx_app)} {tag}: {n_symbols/1e6:.1f} Msym in {wall:.2f} s "
          f"(cpu {cpu:.1f} s, {cpu/wall:.0%}) -> {rate/1e6:.2f} Msym/s "
          f"= {rate/args.target_rate:.0%} of {args.target_rate/1e6:g} Msym/s")
    print(f"  TS: {intact}/{n_ref} input packets recovered intact "
          f"({n_out} output packets); "
          f"snr {stats.get('snr')}, fec frames {fec.get('frames')} "
          f"errors {fec.get('errors')}, avg ldpc trials {fec.get('avg_ldpc_trials')}")
    return 0 if intact > 0.8 * n_ref else 1


if __name__ == "__main__":
    sys.exit(main())
