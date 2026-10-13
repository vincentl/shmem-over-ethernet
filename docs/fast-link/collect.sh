#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §12: collect's places on the cluster (H25 of
# docs/prompts/v0.2-open-items.md), tools/bench/collect-places.sh's measurement
# as the kit runs its jobs: osoe-bench's places - a collect of differing blocks
# over the world, three patterns of lengths, the median and fastest of 200
# collects after 200 more (quick) - by each of collect's four ways, forced
# (OSOE_COLLECTIVE_PLACES: members, totals, scan, exchange), and by the
# default, link, the way the link makes cheapest, the leads' exchange on no
# more than 16 nodes (P35, provisional until this measures it), every node at
# once at each count of PES_PER_NODE, ROUNDS times, the ways in another order
# each round. Every job under SHMEM_DEBUG, whose PE 0 line gives the link the
# default chose from; summarize.py sets the ways' medians side by side, with
# the way shape_timing.py's choose_places takes on each default job's link.
#   ROUNDS (3), PES_PER_NODE ("8 16 32"), WAYS ("members totals scan exchange
#   link")
# usage: collect.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
PES_PER_NODE=${PES_PER_NODE:-8 16 32}
WAYS=${WAYS:-members totals scan exchange link}
for ((round = 1; round <= ROUNDS; round++)); do
    for pes in $PES_PER_NODE; do
        for way in $(rotate "$WAYS" "$round"); do
            launch "$NODE_COUNT" "$pes" "$OUT/places-$way-pes$pes-r$round.txt" \
                OSOE_COLLECTIVE_PLACES="$way" -- "$KIT_BUILD/tools/bench/osoe-bench" places quick
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
