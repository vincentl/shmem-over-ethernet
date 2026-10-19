#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# repeat.sh DIR PRESET N OUT [LOAD]: shmem.death-in-wait.barrier.L4's own command,
# from ctest's listing, run N times in DIR/build/PRESET under the VM's lock; each
# failure's output kept in OUT, and every run's line tally (which thread of each
# survivor spoke: the agent's "ending", the sweep's fatal, the lead's death line)
# in OUT/tally.txt. LOAD busy loops run beside it when given. With TRACE set,
# each run's end trace (make-trace.py) in OUT/trace-N, kept for failures and the
# first three runs.
# DIW_LOCK_HELD: a caller (loaded.sh) holds the lock for the whole of its run.
if [ -z "$DIW_LOCK_HELD" ]; then
    exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
fi
D=$1 P=$2 N=$3 OUT=$4 LOAD=${5:-0}
mkdir -p "$OUT"; rm -rf "$OUT"/run-*.txt "$OUT"/trace-* "$OUT/tally.txt"
cd "$D/build/$P" || exit 1
# The test's command line and working directory, as ctest would run them.
cmd=$(ctest -N -V -R '^shmem\.death-in-wait\.barrier\.L4$' | sed -n 's/^[0-9]*: Test command: //p')
wd=$(ctest -N -V -R '^shmem\.death-in-wait\.barrier\.L4$' | sed -n 's/^[0-9]*: Working Directory: //p')
[ -n "$cmd" ] || { echo "NO COMMAND"; exit 1; }
echo "command: $cmd"; echo "directory: ${wd:-$PWD}"
pids=
for _ in $(seq "$LOAD"); do (while :; do :; done) & pids="$pids $!"; done
pass=0 fail=0
cd "${wd:-$PWD}" || exit 1
for i in $(seq "$N"); do
    start=$(date +%s%N)
    if [ -n "$TRACE" ]; then
        export DIW_TRACE_DIR=$OUT/trace-$i; mkdir -p "$DIW_TRACE_DIR"
    fi
    eval "$cmd" > "$OUT/run-$i.txt" 2>&1
    s=$?
    ms=$(( ($(date +%s%N) - start) / 1000000 ))
    agent=$(grep -c ': ending: the job is aborting' "$OUT/run-$i.txt")
    sweep=$(grep -c ': shmem_barrier_all: the job is aborting' "$OUT/run-$i.txt")
    lead=$(grep -c 'has ended without finalizing' "$OUT/run-$i.txt")
    echo "$i status $s ms $ms agent $agent sweep $sweep lead $lead" >> "$OUT/tally.txt"
    if [ $s = 0 ] && grep -q 'DEATH IN WAIT OK' "$OUT/run-$i.txt"; then
        pass=$((pass + 1)); rm -f "$OUT/run-$i.txt"
        [ -z "$TRACE" ] || [ "$i" -le 3 ] || rm -rf "$OUT/trace-$i"
    else
        fail=$((fail + 1)); echo "run $i failed: $OUT/run-$i.txt"
    fi
done
[ -z "$pids" ] || kill $pids
echo "passed $pass, failed $fail of $N ($P, load $LOAD)"
echo "survivors' lines over every run: agent $(awk '{a+=$7} END {print a}' "$OUT/tally.txt"), sweep $(awk '{a+=$9} END {print a}' "$OUT/tally.txt"), lead $(awk '{a+=$11} END {print a}' "$OUT/tally.txt")"
echo "REPEAT DONE $(date +%H:%M:%S)"
