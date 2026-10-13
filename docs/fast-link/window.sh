#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §14: the pipeline's window and the chooser on a fast link
# (H13 of docs/prompts/v0.2-open-items.md): the large reduction's and
# broadcast's time over the world (shapes.c: 64 KiB to 4 MiB, 4 to 256 of the
# leads' 16 KiB pieces), every node at once at PES_PER_NODE PEs, by the chooser
# - OSOE_COLLECTIVE_SHAPE=link, the default: a tree's fan-out and window or
# Träff's, from the three numbers of the link each job measures at start-up
# (SHMEM_DEBUG) - and by tree, its trees alone, beside every fixed shape of
# FANOUTS and WINDOWS (OSOE_COLLECTIVE_SHAPE=fixed with OSOE_COLLECTIVE_FANOUT
# and OSOE_COLLECTIVE_PIPELINE), ROUNDS times in another order each round:
# where a fixed shape beats the chooser's at a size, the chooser's costs are
# off on this link, which the Pi's table fitted (shape_timing.py).
#   ROUNDS (3), PES_PER_NODE (8), FANOUTS ("2 4 8"), WINDOWS ("1 2 4 8"),
#   TRIPS (100), SIZES (shapes.c's own)
# usage: window.sh NODES OUTDIR
: "${KIT_TIMEOUT:=180}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
PES_PER_NODE=${PES_PER_NODE:-8}
FANOUTS=${FANOUTS:-2 4 8}
WINDOWS=${WINDOWS:-1 2 4 8}
TRIPS=${TRIPS:-100}
SIZES=${SIZES:-}
CASES="link tree"
for k in $FANOUTS; do
    for d in $WINDOWS; do
        CASES="$CASES fixed-k$k-d$d"
    done
done
settings() {
    case $1 in
        link)  echo OSOE_COLLECTIVE_SHAPE=link ;;
        tree)  echo OSOE_COLLECTIVE_SHAPE=tree ;;
        fixed-k*)
            local k=${1#fixed-k}
            echo OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_FANOUT=${k%-d*} \
                 OSOE_COLLECTIVE_PIPELINE=${1#*-d} ;;
    esac
}
for ((round = 1; round <= ROUNDS; round++)); do
    for c in $(rotate "$CASES" "$round"); do
        # shellcheck disable=SC2046,SC2086
        launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/shapes-$c-pes$PES_PER_NODE-r$round.txt" \
            $(settings "$c") -- "$KIT_BIN/shapes" "$TRIPS" $SIZES
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
