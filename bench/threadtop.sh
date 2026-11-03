#!/bin/sh
# Per-thread CPU usage of a command once it has warmed up.
# usage: threadtop.sh <warmup_s> <cmd...>
warm=$1; shift
"$@" >/dev/null 2>&1 &
pid=$!
sleep "$warm"
top -H -b -n 2 -d 3 -p "$pid" | awk '/^top/{n++} n==2' | sed -n '7,20p'
kill "$pid"
