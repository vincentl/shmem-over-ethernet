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
# off on this link, which the Pi's table fitted (shape_timing.py). CASES,
# given, replaces that list: link, tree, link-dD and tree-dD (the chooser, or
# its trees, at a window of D, OSOE_COLLECTIVE_PIPELINE=D) and fixed-kK-dD -
# the third session's four are "link-d4 link-d8 tree-d8 fixed-k4-d8", P37's
# default of 8 timed against the 4 it replaced and the best fixed tree of the
# second session (docs/prompts/done/cluster-session-3-plan.md). The unsuffixed
# link and tree run at the default window, 8 since 2026-10-28 (the first two
# sessions' ran at 4). No case sets OSOE_PROGRESS_HELPERS, so each job runs at
# the library's default of them: none to 2026-10-28, auto since (two on a job
# of three nodes or more whose nodes' PEs fit their CPUs).
#   ROUNDS (3), PES_PER_NODE (8), FANOUTS ("2 4 8"), WINDOWS ("1 2 4 8"),
#   CASES (link, tree and every fixed shape of those), TRIPS (100),
#   SIZES (shapes.c's own)
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
if [ -z "${CASES:-}" ]; then
    CASES="link tree"
    for k in $FANOUTS; do
        for d in $WINDOWS; do
            CASES="$CASES fixed-k$k-d$d"
        done
    done
fi
settings() {
    case $1 in
        link)  echo OSOE_COLLECTIVE_SHAPE=link ;;
        tree)  echo OSOE_COLLECTIVE_SHAPE=tree ;;
        link-d* | tree-d*)
            echo OSOE_COLLECTIVE_SHAPE=${1%-d*} OSOE_COLLECTIVE_PIPELINE=${1#*-d} ;;
        fixed-k*)
            local k=${1#fixed-k}
            echo OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_FANOUT=${k%-d*} \
                 OSOE_COLLECTIVE_PIPELINE=${1#*-d} ;;
        *)     echo "no case $1" >&2; exit 2 ;;
    esac
}
# Every case known before any job runs: an unknown one ends the section here.
for c in $CASES; do settings "$c" > /dev/null; done
for ((round = 1; round <= ROUNDS; round++)); do
    for c in $(rotate "$CASES" "$round"); do
        # shellcheck disable=SC2046,SC2086
        launch "$NODE_COUNT" "$PES_PER_NODE" "$OUT/shapes-$c-pes$PES_PER_NODE-r$round.txt" \
            $(settings "$c") -- "$KIT_BIN/shapes" "$TRIPS" $SIZES
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
