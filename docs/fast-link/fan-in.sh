#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §9: fan-in. Every PE of every node but the first puts into
# the first node's PE 0 at once (fan-in.c), at each count of PES_PER_NODE
# ("1 4 16") and receive buffers (the default 64, and 256), at no helpers and
# at HELPERS' other counts ("0 2"), ROUNDS (3) times; PE 0 prints its node's
# counters before and after the iterations (osoe-stat -d -j), from which
# summarize.py reads the receive pool's -ENOBUFS completions and re-arms. The
# iterations scale with the senders, 320 divided by the PEs a node, so that a
# job moves about as much at one PE a node as at sixteen: at one, twenty
# iterations were some 50 ms of traffic, too little to read a rate from.
# usage: fan-in.sh NODES OUTDIR
: "${KIT_TIMEOUT:=120}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
PES_PER_NODE=${PES_PER_NODE:-1 4 16}
HELPERS=${HELPERS:-0 2}
BUFFERS=${BUFFERS:-64 256}
for ((round = 1; round <= ROUNDS; round++)); do
    for pes in $PES_PER_NODE; do
        for h in $HELPERS; do
            for b in $(rotate "$BUFFERS" "$round"); do
                name=fan-in-pes$pes-h$h-buffers$b-r$round
                launch "$NODE_COUNT" "$pes" "$OUT/$name.txt" OSOE_PROGRESS_HELPERS=$h \
                    OSOE_WIRE_BUFFERS=$b FAN_IN_STAT="$KIT_BUILD/tools/stat/osoe-stat" -- \
                    "$KIT_BIN/fan-in" 65536 16 $((320 / pes))
            done
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
