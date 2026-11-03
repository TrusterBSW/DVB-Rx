#!/bin/sh
# perf profile of one thread (matched by name) of a command once warmed up.
# usage: threadperf.sh <warmup_s> <thread_name_prefix> <cmd...>
warm=$1; name=$2; shift 2
"$@" >/dev/null 2>&1 &
pid=$!
sleep "$warm"
tid=$(ps -L -o tid=,comm= -p "$pid" | awk -v n="$name" 'index($2, n) == 1 {print $1; exit}')
sudo perf record -q -g -t "$tid" -o /tmp/threadperf.data -- sleep 3 >/dev/null 2>&1
kill "$pid"
sudo perf report -i /tmp/threadperf.data --stdio --no-children --sort dso,sym 2>/dev/null |
    grep -E '^ +[0-9]' | head -25
