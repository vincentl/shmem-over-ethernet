#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §8: gets, and the landing zone. get-pieces between the
# list's first two nodes, one PE each, from 16 KiB by fours to LARGEST (64 MiB),
# at each landing zone of ZONES (256 KiB, the default; 1, 4 and 16 MiB); then
# multistream's get stream in 1 MiB blocks over every node, PES_PER_NODE (4) PEs
# each, at the same zones; ROUNDS (3) times in another order each round. The
# reply credits (OSOE_AM_CREDITS, 64 a connection, shared by the node's PEs, a
# piece of 64 KiB holding one until it lands), the other bound on a get's
# pieces in flight, are not varied, and hold a zone back only from a few MiB.
# get-pieces' one PE has at most 64 pieces, 4 MiB, in flight on its one
# connection: at a zone of 4 MiB the credits hold it as the zone does, and at
# 16 MiB they hold it alone. multistream's PEs spread their gets over the
# peers, so that a connection carries about 4/7 of one PE's pieces at eight
# nodes of 4 PEs, and one PE's at five: the credits hold its zone back from
# about 7 MiB at eight nodes, and 4 MiB at five.
# usage: gets.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
ZONES=${ZONES:-262144 1048576 4194304 16777216}
LARGEST=${LARGEST:-67108864}
PES_PER_NODE=${PES_PER_NODE:-4}
SECONDS_EACH=${STREAM_SECONDS:-10}
for ((round = 1; round <= ROUNDS; round++)); do
    for z in $(rotate "$ZONES" "$round"); do
        KIT_SALLOC="$KIT_SALLOC $(pair)" launch 2 1 "$OUT/get-pieces-zone$z-r$round.txt" \
            OSOE_GET_LANDING_BYTES="$z" -- "$KIT_BIN/get-pieces" 21 "$LARGEST"
        launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/get-1m-pes$PES_PER_NODE-zone$z-r$round.txt" \
            OSOE_GET_LANDING_BYTES="$z" -- "$KIT_BIN/multistream" get 1048576 "$SECONDS_EACH"
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
