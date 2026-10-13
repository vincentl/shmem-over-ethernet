#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §13: what waits behind a stream, at each helper count (H15
# to H19 of docs/prompts/v0.2-open-items.md, which §11 once left out). Every
# node at once, two PEs each (hol.c): each node's first PE streams 1 MiB puts to
# the next node's, all but the last node's, while PE 1 times an operation alone
# and then behind the streams - a fetch-add to PE 3, behind PE 0's stream on
# the same connection (H16); the barrier of the nodes' second PEs, whose
# messages go on the control wires the lead's agent keeps at every count (H15);
# and their sum reduction and broadcast of 8 bytes and of 64 KiB, whose
# payloads go on the data wires the helpers hold (H17) - at each count of
# HELPERS, ROUNDS times, the counts in another order each round. And at each
# count a put stream over every node (multistream, 1 MiB blocks, STREAM_PES a
# node), TCP's counters read on every node before and after it (nstat -n, then
# nstat: H18); and the same stream with every node's CPUs profiled
# PROFILE_SECONDS long, PROFILE_DELAY seconds in, beside it (profile.sh, perf by
# PERF; H19) - where perf is missing or refused its file says so, and the
# section goes on.
#   ROUNDS (3), HELPERS ("0 1 2 4"), OPERATIONS ("fetch-add barrier reduce:8
#   reduce:65536 broadcast:8 broadcast:65536"), STREAM_SECONDS (10),
#   STREAM_PES (8), PERF (perf), PROFILE_DELAY (3), PROFILE_SECONDS (5)
# usage: behind.sh NODES OUTDIR
: "${KIT_TIMEOUT:=180}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
HELPERS=${HELPERS:-0 1 2 4}
OPERATIONS=${OPERATIONS:-fetch-add barrier reduce:8 reduce:65536 broadcast:8 broadcast:65536}
SECONDS_EACH=${STREAM_SECONDS:-10}
STREAM_PES=${STREAM_PES:-8}
export PERF=${PERF:-perf}
PROFILE_DELAY=${PROFILE_DELAY:-3}
PROFILE_SECONDS=${PROFILE_SECONDS:-5}
[ "$NODE_COUNT" -ge 5 ] || echo "note: $NODE_COUNT nodes; four helpers need four peers, five nodes" | tee -a "$LOG"
for ((round = 1; round <= ROUNDS; round++)); do
    for h in $(rotate "$HELPERS" "$round"); do
        for o in $OPERATIONS; do
            op=${o%%:*}
            bytes=${o#*:}
            [ "$bytes" = "$o" ] && bytes=
            launch "$NODE_COUNT" 2 "$OUT/hol-$op${bytes:+-$bytes}-h$h-r$round.txt" \
                OSOE_PROGRESS_HELPERS=$h -- "$KIT_BIN/hol" "$SECONDS_EACH" "$op" $bytes
        done
        launch "$NODE_COUNT" 1 "$OUT/nstat-before-h$h-r$round.txt" -- nstat -n
        launch "$NODE_COUNT" "$STREAM_PES" "$OUT/put-1m-pes$STREAM_PES-h$h-r$round.txt" \
            OSOE_PROGRESS_HELPERS=$h -- "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH"
        launch "$NODE_COUNT" 1 "$OUT/nstat-h$h-r$round.txt" -- \
            bash -c 'nstat | sed "s/^/$(hostname) /"'
        BESIDE="bash '$KIT/profile.sh' $PROFILE_SECONDS" BESIDE_DELAY=$PROFILE_DELAY \
            BESIDE_FILE="$OUT/profile-pes$STREAM_PES-h$h-r$round.txt" \
            beside "$NODE_COUNT" "$STREAM_PES" "$OUT/put-1m-profiled-pes$STREAM_PES-h$h-r$round.txt" \
            OSOE_PROGRESS_HELPERS=$h -- "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH"
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
