#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T38: the CPU a wake costs under a trickle of puts, as built and with round
# nine's spin (preflag-mutant.py: a helper and the lead's agent polling up to
# 200 us before they raise their sleeping flag). Takes the VM's lock.
#
#   measure.sh build TAR              the tree twice in /tmp/t38, one mutated; release builds
#   measure.sh run OUT RUNS LOADS PUTS GAP_US [IDLE_MS]
#                                     RUNS rounds of four jobs (as built and the spin, at 0
#                                     and 2 helpers, alternating), beside LOADS busy loops
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=/tmp/t38
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || { echo "the VM's lock was not had in an hour"; exit 1; }

if [ "$1" = build ]; then
    TAR=$2
    rm -rf "${T:?}"
    for tree in built spin; do
        mkdir -p "$T/$tree" && tar -xf "$TAR" -C "$T/$tree" || { echo "no tree"; exit 2; }
    done
    python3 "$HERE/preflag-mutant.py" "$T/spin" || exit 3
    for tree in built spin; do
        ( cd "$T/$tree" && cmake --preset release > "$T/configure-$tree.txt" 2>&1 &&
          cmake --build build/release -j4 --target osoe osoe-launch shmem_test_wake-cost \
            > "$T/build-$tree.txt" 2>&1 ) || { echo "build $tree failed"; tail -20 "$T/build-$tree.txt"; exit 4; }
        echo "built $tree: $(grep -c 'warning:' "$T/build-$tree.txt") warnings"
    done
    exit 0
fi

OUT=$2 RUNS=$3 LOADS=$4 PUTS=$5 GAP=$6 IDLE=${7:-500}
mkdir -p "$OUT"
SPINNERS=""
spin() { for i in $(seq 1 "$LOADS"); do ( exec 9>&-; while :; do :; done ) & SPINNERS="$SPINNERS $!"; done; }
unspin() { for q in $SPINNERS; do kill "$q" 2>/dev/null; done; wait 2>/dev/null; SPINNERS=""; }
trap unspin EXIT
echo "== $(date -u +%FT%TZ) runs=$RUNS loads=$LOADS puts=$PUTS gap=${GAP}us idle=${IDLE}ms; load $(cut -d' ' -f1-3 /proc/loadavg)"
spin
for run in $(seq 1 "$RUNS"); do
    for helpers in 0 2; do
        for tree in built spin; do
            f=$OUT/$tree-h$helpers-l$LOADS-$run.txt
            env OSOE_PROGRESS_HELPERS=$helpers OSOE_DEADLOCK_SECONDS=0 \
                "$T/$tree/build/release/tools/launch/osoe-launch" -n 4 -m 0,1,2,3 -t 60 -- \
                "$T/$tree/build/release/shmem/tests/shmem_test_wake-cost" "$PUTS" "$GAP" "$IDLE" \
                > "$f" 2>&1
            echo "exit $?" >> "$f"
            echo "$tree h$helpers run $run: $(grep -h 'trickle node 0:' "$f" | sed 's/.*puts; //')"
        done
    done
done
unspin
echo "== $(date -u +%FT%TZ) done"
