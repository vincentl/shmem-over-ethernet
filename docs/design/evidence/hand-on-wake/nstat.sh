#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kernel's TCP counters over RUNS turnover-herd runs of each wake (the diagnostic copy).
set -u
T=/var/tmp/hand-on-diag
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || exit 99
command -v nstat > /dev/null || { echo "no nstat"; exit 3; }
map=$(printf '0,%.0s' $(seq 1 "$1"))1
for wake in hand-on all; do
    nstat -n > /dev/null
    long=""
    for ((i = 0; i < $3; i++)); do
        out=$(env SHMEM_SYMMETRIC_SIZE=4m OSOE_TURNOVER_WAKE=$wake $T/build/release/tools/launch/osoe-launch \
              -n $(($1 + 1)) -m "$map" -t 60 -- $T/turnover-herd $2 3 2>&1)
        long="$long $(echo "$out" | grep -oE 'median_longest_put_ms=[0-9.]+' | cut -d= -f2)"
    done
    echo "== $wake: median longest puts:$long"
    nstat | grep -E 'DelayedACK|ZeroWindow|WinProbe|TCPTimeouts|Retrans|TCPAutoCorking|TCPOFOQueue|PruneCalled|RcvPruned|TCPRcvCollapsed|TCPBacklogDrop' | awk '{ printf "  %s %s\n", $1, $2 }'
done
