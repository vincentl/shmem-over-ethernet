#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §8: gets, and the landing zone. get-pieces between the
# list's first two nodes, one PE each, from 16 KiB by fours to LARGEST (64 MiB),
# at each landing zone of ZONES (256 KiB, the default; 1, 4 and 16 MiB); then
# multistream's get stream in 1 MiB blocks over every node, PES_PER_NODE (8) PEs
# each, at the same zones; ROUNDS (3) times in another order each round.
# usage: gets.sh NODES OUTDIR
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
ZONES=${ZONES:-262144 1048576 4194304 16777216}
LARGEST=${LARGEST:-67108864}
PES_PER_NODE=${PES_PER_NODE:-8}
SECONDS_EACH=${STREAM_SECONDS:-10}
for ((round = 1; round <= ROUNDS; round++)); do
    for z in $(rotate "$ZONES" "$round"); do
        launch 2 1 "$OUT/get-pieces-zone$z-r$round.txt" OSOE_GET_LANDING_BYTES="$z" -- \
            "$KIT_BIN/get-pieces" 21 "$LARGEST"
        launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/get-1m-pes$PES_PER_NODE-zone$z-r$round.txt" \
            OSOE_GET_LANDING_BYTES="$z" -- "$KIT_BIN/multistream" get 1048576 "$SECONDS_EACH"
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
