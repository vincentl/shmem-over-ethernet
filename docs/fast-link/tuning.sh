#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §15: latency tuning as a site would make it (H27 of
# docs/prompts/v0.2-open-items.md): the latency-bound cases at the image's
# settings and tuned - interrupt coalescing off and busy polling on, BUSY
# microseconds, on every node (tuning-node.sh, by sudo -n) - the two in another
# order each round, ROUNDS times, and the image's settings put back at the end,
# or when the script is ended. The cases: ping's round trip from the first node
# to the second, 200 at 10 ms; osoe-bench's latency, atomic and barrier, quick,
# one PE on each of the two (osoe's hop); hol's fetch-add alone and behind a 1
# MiB put stream there, two PEs each (HOL_SECONDS); the mailbox on every node at
# PES_PER_NODE (osoe-pacer, MAILBOX_SECONDS); and with FLOOR=1 the floor,
# floor.sh's section whole, into floor-STATE-rROUND/ (its own settings, as
# CONNECTIONS, pass through).
#   ROUNDS (3), BUSY (50), PES_PER_NODE (8), HOL_SECONDS (10),
#   MAILBOX_SECONDS (20), FLOOR (1)
# usage: tuning.sh NODES OUTDIR
: "${KIT_TIMEOUT:=180}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
BUSY=${BUSY:-50}
PES_PER_NODE=${PES_PER_NODE:-8}
HOL_SECONDS=${HOL_SECONDS:-10}
MAILBOX_SECONDS=${MAILBOX_SECONDS:-20}
FLOOR=${FLOOR:-1}
FIRST=$(node 1)
SECOND=$(node 2)
[ -n "$SECOND" ] || { echo "tuning.sh needs two nodes" >&2; exit 2; }
# On one machine both ends are this machine, at its loopback address.
[ "$KIT_LAUNCHER" = osoe-launch ] && SECOND_ADDRESS=127.0.0.1 ||
    SECOND_ADDRESS=$(getent ahostsv4 "$SECOND" | awk 'NR == 1 { print $1 }')
set_state() {   # STATE FILE: every node tuned (on) or as the image has it (off)
    launch "$NODE_COUNT" 1 "$2" -- bash "$KIT/tuning-node.sh" "$1" "$BUSY" "$FIRST"
}
trap 'set_state off "$OUT/tuning-off-end.txt"' EXIT
for ((round = 1; round <= ROUNDS; round++)); do
    for state in $(rotate "default tuned" "$round"); do
        if [ "$state" = tuned ]; then
            set_state on "$OUT/tuning-on-r$round.txt"
        else
            set_state off "$OUT/tuning-off-r$round.txt"
        fi
        { echo "# $(date -u +%H:%M:%S) $FIRST to $SECOND_ADDRESS"
          on "$FIRST" ping -q -c 200 -i 0.01 "$SECOND_ADDRESS" 2>&1
          echo "# exit $?"; } > "$OUT/ping-$state-r$round.txt"
        KIT_SALLOC="$KIT_SALLOC $(pair)" launch 2 1 "$OUT/bench-$state-r$round.txt" -- \
            "$KIT_BUILD/tools/bench/osoe-bench" quick latency atomic barrier
        KIT_SALLOC="$KIT_SALLOC $(pair)" launch 2 2 "$OUT/hol-$state-r$round.txt" -- \
            "$KIT_BIN/hol" "$HOL_SECONDS"
        launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/mailbox-$state-pes$PES_PER_NODE-r$round.txt" -- \
            "$KIT_BUILD/tools/pacer/osoe-pacer" --seconds "$MAILBOX_SECONDS" mailbox
        if [ "$FLOOR" = 1 ]; then
            bash "$KIT/floor.sh" "$NODES_FILE" "$OUT/floor-$state-r$round" \
                > "$OUT/floor-$state-r$round.log" 2>&1
            echo "$(date -u +%H:%M:%S) exit $? floor-$state-r$round: floor.sh" >> "$LOG"
        fi
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
