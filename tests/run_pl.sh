#!/bin/bash
# Full symbol-level receiver (PL sync -> TS) on each vector set, with noise
# near the QEF threshold and a carrier offset (cycles/symbol).
# usage: run_pl.sh [freq] [extra EsN0 margin dB]
cd "$(dirname "$0")/.."
freq=${1:-0.01}
margin=${2:-0.5}
while read -r dir esn0; do
    snr=$(awk -v a="$esn0" -v b="$margin" 'BEGIN { print a + b }')
    printf '%-10s EsN0 %-5s f %s: ' "$dir" "$snr" "$freq"
    libdvbrx/build/dvbrx_file -n "$snr" -f "$freq" "work/vec/$dir/plframe.fc32" \
        "work/vec/$dir/rx.ts" 2>&1 | tr '\n' ' ' |
        grep -oE 'Msym/s\)|SNR [-0-9.]+ dB|freq [-0-9.]+|plframes [0-9]+|failed [0-9]+, avg [0-9.]+ it|crc errors' | tr '\n' ' '
    python3 tests/tscompare.py work/ts1m.ts "work/vec/$dir/rx.ts"
done <<'LIST'
qpsk34 4.0
8psk3_5 5.5
8psk3_4 7.9
16apsk3_4 10.2
32apsk4_5 13.6
LIST
