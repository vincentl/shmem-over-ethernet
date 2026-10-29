#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P2's timing in the osoe VM: turnover-herd.c - every PE of node 0 putting to node 1's one PE -
# at WRITERS writers and BLOCKS-byte blocks, OSOE_TURNOVER_WAKE=all against hand-on, their
# order alternated each series, SECONDS each, no helpers (a pair of nodes: auto gives none).
#   bench.sh TREE OUT [SERIES] [SECONDS] [WRITERS] [BLOCKS]
set -u
TREE=$1
OUT=$2
SERIES=${3:-3}
SECONDS_EACH=${4:-5}
WRITERS=${5:-4 8 16 32 64}
BLOCKS=${6:-1048576 65536}
HERE=$(cd "$(dirname "$0")" && pwd)
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || { echo "the VM's lock was not had in an hour"; exit 99; }
mkdir -p "$OUT"
B=$TREE/build/release
"$B/tools/wrapper/oshcc" -O2 -I "$TREE/shmem/tests" -I "$TREE/tools/stat" -o "$OUT/turnover-herd" \
    "$HERE/turnover-herd.c" "$TREE/tools/stat/segment.c" > "$OUT/compile.txt" 2>&1 ||
    { echo "turnover-herd did not compile"; cat "$OUT/compile.txt"; exit 3; }
LOG=$OUT/herd.txt
{
    echo "# $(date -u +%FT%TZ) $TREE, series $SERIES, $SECONDS_EACH s each, writers $WRITERS, blocks $BLOCKS"
    echo "# load $(cut -d' ' -f1-3 /proc/loadavg); $(nproc) CPUs; $(free -m | awk '/^Mem:/ { print $7 " MiB available" }')"
} >> "$LOG"
for ((s = 1; s <= SERIES; s++)); do
    order="all hand-on"
    [ $((s % 2)) -eq 0 ] && order="hand-on all"
    for w in $WRITERS; do
        map=$(printf '0,%.0s' $(seq 1 "$w"))1
        for b in $BLOCKS; do
            for wake in $order; do
                line=$(env SHMEM_SYMMETRIC_SIZE=4m OSOE_TURNOVER_WAKE="$wake" \
                       "$B/tools/launch/osoe-launch" -n $((w + 1)) -m "$map" -t 120 -- \
                       "$OUT/turnover-herd" "$b" "$SECONDS_EACH" 2>&1)
                status=$?
                herd=$(echo "$line" | grep '^herd ')
                echo "series=$s exit=$status ${herd:-no herd line: $(echo "$line" | tail -2 | tr '\n' ' ')}" >> "$LOG"
            done
        done
    done
done
echo "# $(date -u +%T) done" >> "$LOG"
