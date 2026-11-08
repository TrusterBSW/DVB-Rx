#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Bridge (/dev/dvb) end to end with a virtual radio (-F): one test daemon on adapter 1 per case,
# tuned by dvbv5-zap (as tvheadend would), its TS recorded for 15 s and checked. As root, with the
# module loaded and nothing else on adapter 1 (systemctl stop dvbrxd@1):
#   sudo tests/bridge_file.sh            IN=DIR: sym/ and vec/ as for regress.py (default work/regress-in)
# BIN=PATH tests another build of dvbrx. The few TS errors on DVB-S and DVB-C come from the loop
# joint of the file.
set -u
cd "$(dirname "$0")/.."
IN=${IN:-work/regress-in}; T=work/bridge; mkdir -p $T
BIN=${BIN:-libdvbrx/build/dvbrx}
mk() { # NAME SRC ROLLOFF ESN0 SPS FREQ
  [ -f $T/$1.fc32 ] && return
  python3 tests/txshape.py $2 $T/sh.fc32 --rolloff $3 --esn0 $4 --freq $6 --sps 4 >/dev/null
  python3 tests/channel.py $T/sh.fc32 $T/$1.fc32 --in-rate 4 --out-rate $5 --ppm 15 --snr 60 >/dev/null
  rm -f $T/sh.fc32
}
probe() { # FILE EXTRA_ARGS CHANNEL_CONF_BODY
  $BIN -d 1 -F $T/$1.fc32 -t 2 $2 >$T/$1.log 2>&1 & p=$!
  sleep 3
  printf '[%s]\n%b' "$1" "$3" > $T/$1.conf
  timeout 40 dvbv5-zap -a 1 -c $T/$1.conf -P -o $T/$1.ts -t 15 "$1" >$T/$1.zap 2>&1
  grep -E "Lock|Layer|C/N|signal" $T/$1.zap | tail -2 | sed 's/^/    zap: /'
  python3 - $T/$1.ts <<'PY'
import sys
b = open(sys.argv[1], "rb").read(); n = len(b) // 188
bad = sum(1 for k in range(n) if b[k * 188] != 0x47)
tei = sum(1 for k in range(n) if b[k * 188] == 0x47 and b[k * 188 + 1] & 0x80)
print("    TS: %d packets in 15 s, %d out of sync, %d marked in error" % (n, bad, tei))
PY
  grep -E "Msps|lock|SNR" $T/$1.log | tail -3 | sed 's/^/    log: /'
  kill -INT $p; wait $p 2>/dev/null
}
mk s78  $IN/sym/dvbs_78.fc32 0.35 10 2.0 0.01
mk c64  $IN/sym/dvbc_64.fc32 0.15 24 2.0 0.005
mk s2p8 $IN/vec/8psk3_4/plframe.fc32 0.20 9 2.0 0.002
mk s2q05 $IN/vec/qpsk34/plframe.fc32 0.05 5 1.17 0.002
echo "== DVB-S 7/8, 10 Msym/s, 2 sps"
probe s78 "" '\tDELIVERY_SYSTEM = DVBS\n\tFREQUENCY = 1200000\n\tSYMBOL_RATE = 10000000\n\tINNER_FEC = AUTO\n\tPOLARIZATION = HORIZONTAL\n'
echo "== DVB-C 64-QAM, 6.875 Msym/s, 2 sps"
probe c64 "" '\tDELIVERY_SYSTEM = DVBC/ANNEX_A\n\tFREQUENCY = 650000000\n\tSYMBOL_RATE = 6875000\n\tMODULATION = QAM/AUTO\n\tINNER_FEC = AUTO\n'
echo "== DVB-S2 8PSK 3/4 ro 0.20, 2 sps"
probe s2p8 "" '\tDELIVERY_SYSTEM = DVBS2\n\tFREQUENCY = 1200000\n\tSYMBOL_RATE = 10000000\n\tINNER_FEC = AUTO\n\tMODULATION = PSK/8\n\tROLLOFF = 20\n\tPOLARIZATION = HORIZONTAL\n'
echo "== DVB-S2 QPSK 3/4 ro 0.05, 1.17 sps (-S 11.7e6)"
probe s2q05 "-S 11.7e6" '\tDELIVERY_SYSTEM = DVBS2\n\tFREQUENCY = 1200000\n\tSYMBOL_RATE = 10000000\n\tINNER_FEC = AUTO\n\tMODULATION = QPSK\n\tROLLOFF = 5\n\tPOLARIZATION = HORIZONTAL\n'
