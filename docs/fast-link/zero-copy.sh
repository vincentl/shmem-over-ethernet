#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §7: the zero-copy send. multistream's put stream in 1 MiB
# and 64 KiB blocks over every node, PES_PER_NODE (4) PEs each, with
# OSOE_WIRE_ZERO_COPY off, and on at a threshold of 16 KiB (the default),
# 64 KiB and 256 KiB; at each count of HELPERS ("0", and §5's best beside it),
# ROUNDS (3) times in another order each round, PE 0 printing its node's
# counters around each window (its sends, those by reference among them - a
# refused one counted there too - their notifications, and those the kernel
# copied after all, a refused one's among them). The locked-memory limit each
# node's steps see is recorded first: below what the lent pages need, a send is
# refused, goes again by copy, and its connection copies from then on, which
# each process says once ("refused for want of locked memory"). HELPERS is "0"
# unless given: set it to "0 N", N §5's best, for the runbook's 48 jobs.
# usage: zero-copy.sh NODES OUTDIR
: "${KIT_TIMEOUT:=120}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
HELPERS=${HELPERS:-0}
PES_PER_NODE=${PES_PER_NODE:-4}
SECONDS_EACH=${STREAM_SECONDS:-10}
launch "$NODE_COUNT" 1 "$OUT/memlock.txt" -- bash -c 'echo "$(hostname) ulimit -l $(ulimit -l)"'
CASES="off on-16k on-64k on-256k"
settings() {
    case $1 in
        off)     echo OSOE_WIRE_ZERO_COPY=off ;;
        on-16k)  echo OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=16384 ;;
        on-64k)  echo OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=65536 ;;
        on-256k) echo OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=262144 ;;
    esac
}
for ((round = 1; round <= ROUNDS; round++)); do
    for h in $HELPERS; do
        for c in $(rotate "$CASES" "$round"); do
            for bytes in 1048576 65536; do
                # shellcheck disable=SC2046
                launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/put-$bytes-$c-pes$PES_PER_NODE-h$h-r$round.txt" \
                    $(settings "$c") OSOE_PROGRESS_HELPERS=$h \
                    MULTISTREAM_STAT="$KIT_BUILD/tools/stat/osoe-stat" -- \
                    "$KIT_BIN/multistream" put "$bytes" "$SECONDS_EACH"
            done
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
