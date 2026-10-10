#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §10: the cluster at size. osoe-bench over every node at each
# count of PES_PER_NODE ("8 16") and HELPERS ("0", and §5's best beside it);
# osoe-pacer's mix for MIX_SECONDS (600) at each helper count, 4 PEs a node;
# start-up at 1, 8 and 32 PEs a node; and, when SOS_BUILD names a build of the
# SOS test suite (docs/pi-cluster.md §8), its programs at 2 PEs a node.
# usage: scale.sh NODES OUTDIR
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
PES_PER_NODE=${PES_PER_NODE:-8 16}
HELPERS=${HELPERS:-0}
MIX_SECONDS=${MIX_SECONDS:-600}
for h in $HELPERS; do
    for pes in $PES_PER_NODE; do
        launch "$NODE_COUNT" "$pes" "$OUT/bench-pes$pes-h$h.txt" OSOE_PROGRESS_HELPERS=$h -- \
            "$KIT_BUILD/tools/bench/osoe-bench"
    done
    KIT_TIMEOUT=$((MIX_SECONDS + 300)) launch "$NODE_COUNT" 4 "$OUT/mix-h$h.txt" \
        OSOE_PROGRESS_HELPERS=$h -- "$KIT_BUILD/tools/pacer/osoe-pacer" --seconds "$MIX_SECONDS" mix
done
for pes in 1 8 32; do
    for round in 1 2 3; do
        launch "$NODE_COUNT" "$pes" "$OUT/startup-pes$pes-r$round.txt" -- "$KIT_BIN/startup"
    done
done
if [ -n "${SOS_BUILD:-}" ]; then
    for program in $(find "$SOS_BUILD/test" -type f -perm -u+x -name '*' | sort); do
        name=$(echo "${program#"$SOS_BUILD"/test/}" | tr '/' '-')
        KIT_TIMEOUT=120 launch "$NODE_COUNT" 2 "$OUT/sos-$name.txt" -- "$program"
    done
else
    echo "SOS_BUILD is not set: no SOS run" >> "$LOG"
fi
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
