#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 0: profile the node leads' agents under a put stream and the
# mailbox pattern, and the bare io_uring stream beside them. Over loopback, two
# logical nodes in the VM. Takes the VM lock.
# usage: profile.sh NAME OUT   (NAME: the build in /tmp/ladder/NAME; OUT: results)
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
NAME=$1; OUT=$2; mkdir -p $OUT
R=/tmp/ladder/$NAME/build/release
L=$R/tools/launch/osoe-launch
STAT=$R/tools/stat/osoe-stat
S=<repo>/build/scratch/ladder
W=/tmp/ladder/$NAME/work; rm -rf $W; mkdir -p $W
: > $OUT/index.txt
$R/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c > $OUT/oshcc.txt 2>&1 \
    || { echo "OSHCC FAILED"; cat $OUT/oshcc.txt; exit 1; }

# The agent threads of a program's node leads (OSOE_LOCALID 0), comma-separated.
leads() {
    for p in $(pgrep -x "$1"); do
        local id
        id=$(tr '\0' '\n' < /proc/$p/environ | sed -n 's/^OSOE_LOCALID=//p')
        [ "$id" = 0 ] || continue
        ps -L -o tid=,comm= -p $p | awk '$2=="osoe-agent"{print $1}'
    done | paste -sd, -
}
pids() { pgrep -x "$1" | paste -sd, -; }

# The windows over a running job: per-thread CPU and the library's counters
# (5 s), the leads' agents with DWARF stacks (6 s), the whole machine (4 s).
windows() {
    local name=$1 prog=$2 agents=$3
    local P; P=$(pids "$prog")
    local T=""; [ "$agents" = yes ] && T=$(leads "$prog")
    echo "$name start=$(date +%T) pids=$P agents=$T" >> $OUT/index.txt
    local segs=""
    if [ "$agents" = yes ]; then
        segs=$($STAT -l 2>/dev/null | awk '$1 ~ /osoe\./ {print $1}' | grep -v tombstone)
        echo "$name segments=$(echo $segs)" >> $OUT/index.txt
        $STAT -c -j $segs > $W/$name.stat0.json 2>&1
    fi
    sudo perf stat --per-thread -e task-clock -p $P -- sleep 5 2> $OUT/$name.cpu.txt
    if [ "$agents" = yes ]; then
        $STAT -c -j $segs > $W/$name.stat1.json 2>&1
        $STAT -T $segs > $OUT/$name.threads.txt 2>&1
        sudo perf record -q -e cpu-clock -F 499 --call-graph dwarf,16384 -t $T \
            -o $W/$name.agents.data -- sleep 6 2>> $OUT/$name.perf.err
    else
        sudo perf record -q -e cpu-clock -F 499 --call-graph dwarf,16384 -p $P \
            -o $W/$name.agents.data -- sleep 6 2>> $OUT/$name.perf.err
    fi
    sudo perf record -q -a -e cpu-clock -F 499 -g -o $W/$name.all.data -- sleep 4 \
        2>> $OUT/$name.perf.err
    echo "$name end=$(date +%T)" >> $OUT/index.txt
}

# After the job: the samples as text, for classify.py on the host.
convert() {
    local name=$1
    sudo perf script -i $W/$name.agents.data -F comm,tid,ip,sym,dso 2>/dev/null \
        > $OUT/$name.agents.script
    sudo perf script -i $W/$name.all.data -F comm,tid,ip,sym,dso 2>/dev/null \
        > $OUT/$name.all.script
    sudo perf report -i $W/$name.all.data --stdio --no-children --sort comm \
        2>/dev/null | grep -v '^#' | grep -v '^$' | head -20 > $OUT/$name.all.comm.txt
    [ -f $W/$name.stat0.json ] && cp $W/$name.stat0.json $W/$name.stat1.json $OUT/ 2>/dev/null
    true
}

echo "== clean put stream, 64 KiB, 20 s $(date +%T)"
$L -n 2 -m 0,1 -t 60 -- $W/putstream 65536 20 > $OUT/put64-clean.txt 2>&1
tail -1 $OUT/put64-clean.txt

echo "== profiled put stream, 64 KiB, 30 s $(date +%T)"
$L -n 2 -m 0,1 -t 90 -- $W/putstream 65536 30 > $OUT/put64.txt 2>&1 &
JOB=$!
sleep 6
windows put64 putstream yes
wait $JOB
convert put64
tail -1 $OUT/put64.txt

echo "== profiled put stream, 1 MiB, 30 s $(date +%T)"
$L -n 2 -m 0,1 -t 90 -- $W/putstream 1048576 30 > $OUT/put1m.txt 2>&1 &
JOB=$!
sleep 6
windows put1m putstream yes
wait $JOB
convert put1m
tail -1 $OUT/put1m.txt

echo "== profiled mailbox, 4 PEs on 2 nodes, 30 s $(date +%T)"
$L -n 4 -m 0,0,1,1 -t 90 -- $R/tools/pacer/osoe-pacer --seconds 30 --report 5 mailbox \
    > $OUT/mailbox.txt 2>&1 &
JOB=$!
sleep 6
windows mailbox osoe-pacer yes
wait $JOB
convert mailbox
grep -E '^summary|failures=' $OUT/mailbox.txt

echo "== profiled bare io_uring stream $(date +%T)"
stdbuf -oL $R/tools/echo-uring/echo-uring --rounds 10 --mebibytes 400000 > $OUT/echo.txt 2>&1 &
JOB=$!
sleep 2
windows echo echo-uring no
for i in $(seq 60); do grep -q '^uring stream_mib_per_s' $OUT/echo.txt && break; sleep 1; done
for p in $(pgrep -x echo-uring); do kill $p; done
wait $JOB 2>/dev/null
convert echo
grep -E 'stream_mib_per_s' $OUT/echo.txt | head -3

echo "DONE $(date +%T)"
