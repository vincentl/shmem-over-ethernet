#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §19: H14 of docs/prompts/v0.2-open-items.md - what the
# link's measurement at start-up costs on two and three nodes, where, when it
# was found, no fan-out differed and so no choice read it; since step 3's
# Träff's and step 4's unsent rule the chooser and every data wire's unsent
# limit do. The start-up probe on the list's first nodes, at each count of
# NODE_COUNTS and PES_PER_NODE, with the link measured - the default - and set
# by OSOE_LINK_* to what a first job measured, so that the two do the same but
# the measurement; ROUNDS rounds, the two in another order each round.
#   NODE_COUNTS ("2 3"), PES_PER_NODE ("1 8"), ROUNDS (5)
# usage: link-cost.sh NODES OUTDIR
: "${KIT_TIMEOUT:=120}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
NODE_COUNTS=${NODE_COUNTS:-2 3}
PES_PER_NODE=${PES_PER_NODE:-1 8}
ROUNDS=${ROUNDS:-5}
# first COUNT: salloc's option for the list's first COUNT nodes.
first() { [ "$KIT_LAUNCHER" = osoe-launch ] || echo "-w $(head -n "$1" "$NODES_FILE" | paste -sd,)"; }
smallest=$(printf '%s\n' $NODE_COUNTS | sort -n | head -1)
KIT_SALLOC="$KIT_SALLOC $(first "$smallest")" \
    launch "$smallest" 1 "$OUT/link-first.txt" -- "$KIT_BIN/startup"
link=$(sed -n 's/^osoe: PE 0 of .*; link measured: a hop of \([0-9]*\) ns, a message cost of \([0-9]*\) ns, \([0-9]*\) bytes a second.*/OSOE_LINK_HOP_NS=\1 OSOE_LINK_MESSAGE_NS=\2 OSOE_LINK_BYTES_PER_SECOND=\3/p' \
    "$OUT/link-first.txt" | head -1)
[ -n "$link" ] || { echo "the first job measured no link (SHMEM_DEBUG's line)" | tee -a "$LOG"; exit 1; }
echo "# the link set: $link" >> "$LOG"
for ((round = 1; round <= ROUNDS; round++)); do
    for nodes in $NODE_COUNTS; do
        for pes in $PES_PER_NODE; do
            for how in $(rotate "measured set" "$round"); do
                settings=()
                [ "$how" = measured ] || read -ra settings <<< "$link"
                KIT_SALLOC="$KIT_SALLOC $(first "$nodes")" \
                    launch "$nodes" "$pes" "$OUT/startup-$how-nodes$nodes-pes$pes-r$round.txt" \
                    "${settings[@]}" -- "$KIT_BIN/startup"
            done
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
