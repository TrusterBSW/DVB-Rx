#!/bin/bash
# Back-end chain (demap -> LDPC -> BCH -> BB) on each set of vectors, at an
# Es/N0 about 0.5 dB above the DVB-S2 QEF threshold.
# usage: run_backend.sh [extra test_backend args, e.g. estimate]
cd "$(dirname "$0")/.."
while read -r dir const rate esn0; do
    libdvbrx/build/test_backend "work/vec/$dir/xfecframe.fc32" "$const" "$rate" "$esn0" \
        "work/vec/$dir/out.ts" "$@"
    python3 tests/tscompare.py work/ts1m.ts "work/vec/$dir/out.ts"
done <<'LIST'
qpsk34 QPSK 3_4 4.5
8psk3_5 8PSK 3_5 6.0
8psk3_4 8PSK 3_4 8.5
16apsk3_4 16APSK 3_4 10.7
32apsk4_5 32APSK 4_5 14.0
LIST
