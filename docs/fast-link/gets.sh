#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §8: gets, and the landing zone. get-pieces between the
# list's first two nodes, one PE each, from 16 KiB by fours to LARGEST (64 MiB),
# at each landing zone of ZONES; then multistream's get stream in 1 MiB blocks
# over every node at each count of PES_PER_NODE, at the same zones; ROUNDS (3)
# times in another order each round. ZONES are OSOE_GET_LANDING_BYTES's values,
# and "default" the zone unset: a node's 4 MiB split among its PEs, between 256
# KiB and 1 MiB a PE (P32) - 1 MiB at one to four PEs a node, 512 KiB at eight,
# 256 KiB at sixteen - which H28 measures beside 256 KiB and 1 MiB fixed, at 1,
# 4, 8 and 16 PEs a node, the defaults here; the first session's 4 and 16 MiB
# (H9) are ZONES="262144 1048576 4194304 16777216" PES_PER_NODE=8. The
# variable is unset here, so that "default" is the library's own. The reply
# credits (OSOE_AM_CREDITS, 64 a connection, shared by the node's PEs, a
# piece of 64 KiB holding one until it lands), the other bound on a get's
# pieces in flight, are not varied, and hold a zone back only from a few MiB.
# get-pieces' one PE has at most 64 pieces, 4 MiB, in flight on its one
# connection: at a zone of 4 MiB the credits hold it as the zone does, and at
# 16 MiB they hold it alone. multistream's PEs spread their gets over the
# peers, so that a connection carries about 8/7 of one PE's pieces at eight
# nodes of 8 PEs, and two PEs' at five: the credits hold its zone back from
# about 3.5 MiB at eight nodes, and 2 MiB at five. A default case's files say
# no zone; each PE's start-up line (SHMEM_DEBUG) names the zone it had.
# usage: gets.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
unset OSOE_GET_LANDING_BYTES
ROUNDS=${ROUNDS:-3}
ZONES=${ZONES:-default 262144 1048576}
LARGEST=${LARGEST:-67108864}
PES_PER_NODE=${PES_PER_NODE:-1 4 8 16}
SECONDS_EACH=${STREAM_SECONDS:-10}
for ((round = 1; round <= ROUNDS; round++)); do
    for z in $(rotate "$ZONES" "$round"); do
        zone=-zone$z setting=OSOE_GET_LANDING_BYTES=$z
        [ "$z" = default ] && zone= setting=
        # shellcheck disable=SC2086
        KIT_SALLOC="$KIT_SALLOC $(pair)" launch 2 1 "$OUT/get-pieces$zone-r$round.txt" \
            $setting -- "$KIT_BIN/get-pieces" 21 "$LARGEST"
        for pes in $PES_PER_NODE; do
            # shellcheck disable=SC2086
            launch "$NODE_COUNT" "$pes" "$OUT/get-1m-pes$pes$zone-r$round.txt" \
                $setting -- "$KIT_BIN/multistream" get 1048576 "$SECONDS_EACH"
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
