#!/bin/sh
# Steady-state receiver throughput: runs a receiver on a looping IQ file and
# converts the decoded FEC frame rate into symbols per second.
# usage: steady.sh <rx_app> <iq.fc32> <modcod> <symbols_per_plframe> [extra rx args]
app=$1; iq=$2; modcod=$3; spf=$4; shift 4
log=$(mktemp)
timeout 14 "$app" --source file --in-file "$iq" --in-repeat --sink file \
    --out-file /dev/null -m "$modcod" -p on -s 1e6 -o 2 --log-all --log-period 2 \
    "$@" > "$log" 2>&1
grep -o '"fec": {"frames": [0-9]*' "$log" | grep -o '[0-9]*$' | awk -v spf="$spf" '
    { f[NR] = $1 } END {
        # skip the first two reports (start-up), rate over the rest
        if (NR < 4) { print "not enough reports"; exit 1 }
        r = (f[NR] - f[3]) / ((NR - 3) * 2)
        printf "%.0f frames/s -> %.2f Msym/s\n", r, r * spf / 1e6 }'
grep '"lock"' "$log" | tail -n 1 | grep -oE '"snr": [^,]*|"freq_offset_hz": [^,]*|"rejected": [0-9]*|"errors": [0-9]*|"avg_ldpc_trials": [0-9]*' | tr '\n' ' '
echo
rm -f "$log"
