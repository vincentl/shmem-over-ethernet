#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §5: the progress helpers. Every node at once, each case a
# fresh job at one helper count: multistream's put stream in 1 MiB and 64 KiB
# blocks and its get stream in 1 MiB blocks, ten seconds each, and
# osoe-pacer's mailbox for twenty, at each count of HELPERS and PES_PER_NODE,
# ROUNDS times, the helper counts in another order each round.
#   ROUNDS (3), HELPERS ("0 1 2 4"), PES_PER_NODE ("8 16 32"),
#   WORKLOADS ("put-1m put-64k get-1m mailbox"), STREAM_SECONDS (10)
# usage: helpers.sh NODES OUTDIR
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
HELPERS=${HELPERS:-0 1 2 4}
PES_PER_NODE=${PES_PER_NODE:-8 16 32}
WORKLOADS=${WORKLOADS:-put-1m put-64k get-1m mailbox}
SECONDS_EACH=${STREAM_SECONDS:-10}
[ "$NODE_COUNT" -ge 5 ] || echo "note: $NODE_COUNT nodes; four helpers need four peers, five nodes" | tee -a "$LOG"
for ((round = 1; round <= ROUNDS; round++)); do
    for pes in $PES_PER_NODE; do
        for h in $(rotate "$HELPERS" "$round"); do
            for w in $WORKLOADS; do
                f=$OUT/$w-pes$pes-h$h-r$round.txt
                case $w in
                    put-1m)  launch "$NODE_COUNT" "$pes" "$f" OSOE_PROGRESS_HELPERS=$h -- \
                                 "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH" ;;
                    put-64k) launch "$NODE_COUNT" "$pes" "$f" OSOE_PROGRESS_HELPERS=$h -- \
                                 "$KIT_BIN/multistream" put 65536 "$SECONDS_EACH" ;;
                    get-1m)  launch "$NODE_COUNT" "$pes" "$f" OSOE_PROGRESS_HELPERS=$h -- \
                                 "$KIT_BIN/multistream" get 1048576 "$SECONDS_EACH" ;;
                    mailbox) launch "$NODE_COUNT" "$pes" "$f" OSOE_PROGRESS_HELPERS=$h -- \
                                 "$KIT_BUILD/tools/pacer/osoe-pacer" --seconds $((2 * SECONDS_EACH)) mailbox ;;
                    *)       echo "no workload $w" >&2; exit 2 ;;
                esac
            done
        done
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
