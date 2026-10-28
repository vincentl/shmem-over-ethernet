#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §20: many PEs a node (H12 of docs/prompts/v0.2-open-items.md),
# between the list's first two nodes, so that every PE of a node sends toward
# one other node at once: the case in which each reopening of a connection's
# buffer wakes every writer asleep for its room (the release fixes' review,
# S1), which P2's wake of one would replace, and in which a node's lead
# compares the most calls when OSOE_CHECK_COLLECTIVES is on (P24). At each
# count of PES_PER_NODE, helpers.sh's workloads - multistream's put stream in
# 1 MiB and 64 KiB blocks and its get stream in 1 MiB blocks, STREAM_SECONDS
# each, PE 0 printing its node's counters around each window (osoe-stat, the
# reservations that waited for room among them), osoe-pacer's mailbox for
# twice that, and osoe-bench's barrier and collectives, quick - with the
# collectives run twice, OSOE_CHECK_COLLECTIVES off and on. Then at WAKE_PES a
# node the wake's cost alone: the two put streams, the 1 MiB one again with
# both nodes' CPUs profiled PROFILE_SECONDS long, PROFILE_DELAY seconds in
# (profile.sh, by PERF: the turnover's wake is the progress thread's
# futex_wake), and the collectives with the check off and on. Each multistream
# node line gives its PEs' sleeps a 64 KiB moved, their voluntary context
# switches over the window: S1 measured 0.75 at one writer, 8.4 to 9.1 at 16
# and 16 to 23 at 32 under the rule then, and 1.0 with a wake of one. Every job
# has the heap HEAP (SHMEM_SYMMETRIC_SIZE), the same at every count: what a PE
# holds in /dev/shm is its heap and the library's region beside it, about
# 15.6 MiB at the defaults, so that 256 PEs ask about 7.9 GiB of a node at
# 16 MiB, which a c4-standard-16's 29 GiB holds and the development VM's 3.9
# GiB does not (its dry run takes WAKE_PES=64). Everything at HELPERS, none by
# default: a pair has one peer, which one helper holds whatever the count, and
# OSOE_PROGRESS_HELPERS=auto gives a pair none. One round, the author's
# (docs/decisions/2026-10-27.md); with ROUNDS more, the check's two runs change
# order each round.
#   PES_PER_NODE ("16 32 64"), WAKE_PES (256; empty, none), HELPERS ("0"),
#   HEAP (16m), STREAM_SECONDS (10), WORKLOADS ("put-1m put-64k get-1m mailbox
#   collectives"), PERF (perf), PROFILE_DELAY (5), PROFILE_SECONDS (5),
#   WAKE_TIMEOUT (600), ROUNDS (1)
# usage: many-pes.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
PES_PER_NODE=${PES_PER_NODE:-16 32 64}
WAKE_PES=${WAKE_PES-256}
HELPERS=${HELPERS:-0}
HEAP=${HEAP:-16m}
SECONDS_EACH=${STREAM_SECONDS:-10}
WORKLOADS=${WORKLOADS:-put-1m put-64k get-1m mailbox collectives}
export PERF=${PERF:-perf}
PROFILE_DELAY=${PROFILE_DELAY:-5}
PROFILE_SECONDS=${PROFILE_SECONDS:-5}
WAKE_TIMEOUT=${WAKE_TIMEOUT:-600}
ROUNDS=${ROUNDS:-1}
[ -n "$(node 2)" ] || { echo "many-pes.sh needs two nodes" >&2; exit 2; }
STAT=$KIT_BUILD/tools/stat/osoe-stat
# Every job: the pair, and the heap.
pair_job() {   # PES OUTFILE [NAME=VALUE...] -- PROGRAM [ARGUMENTS...]
    local pes=$1 file=$2
    shift 2
    KIT_SALLOC="$KIT_SALLOC $(pair)" launch 2 "$pes" "$file" SHMEM_SYMMETRIC_SIZE="$HEAP" "$@"
}
collectives() {   # PES HELPERS ROUND: osoe-bench's lines with the check off and on
    local check value
    for check in $(rotate "off on" $(($3 - 1))); do
        [ "$check" = on ] && value=1 || value=0
        pair_job "$1" "$OUT/collectives-check-$check-pes$1-h$2-r$3.txt" \
            OSOE_PROGRESS_HELPERS="$2" OSOE_CHECK_COLLECTIVES=$value -- \
            "$KIT_BUILD/tools/bench/osoe-bench" quick barrier collectives
    done
}
for ((round = 1; round <= ROUNDS; round++)); do
    for pes in $PES_PER_NODE; do
        for h in $HELPERS; do
            for w in $WORKLOADS; do
                f=$OUT/$w-pes$pes-h$h-r$round.txt
                case $w in
                    put-1m)  pair_job "$pes" "$f" OSOE_PROGRESS_HELPERS="$h" MULTISTREAM_STAT="$STAT" -- \
                                 "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH" ;;
                    put-64k) pair_job "$pes" "$f" OSOE_PROGRESS_HELPERS="$h" MULTISTREAM_STAT="$STAT" -- \
                                 "$KIT_BIN/multistream" put 65536 "$SECONDS_EACH" ;;
                    get-1m)  pair_job "$pes" "$f" OSOE_PROGRESS_HELPERS="$h" MULTISTREAM_STAT="$STAT" -- \
                                 "$KIT_BIN/multistream" get 1048576 "$SECONDS_EACH" ;;
                    mailbox) pair_job "$pes" "$f" OSOE_PROGRESS_HELPERS="$h" -- \
                                 "$KIT_BUILD/tools/pacer/osoe-pacer" --seconds $((2 * SECONDS_EACH)) mailbox ;;
                    collectives) collectives "$pes" "$h" "$round" ;;
                    *)       echo "no workload $w" >&2; exit 2 ;;
                esac
            done
        done
    done
    # The wake's cost at WAKE_PES a node: the streams whose writers share one
    # connection, one of them profiled beside it, and the check at that count.
    [ -n "$WAKE_PES" ] || continue
    for h in $HELPERS; do
        KIT_TIMEOUT=$WAKE_TIMEOUT pair_job "$WAKE_PES" "$OUT/put-1m-pes$WAKE_PES-h$h-r$round.txt" \
            OSOE_PROGRESS_HELPERS="$h" MULTISTREAM_STAT="$STAT" -- \
            "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH"
        KIT_TIMEOUT=$WAKE_TIMEOUT pair_job "$WAKE_PES" "$OUT/put-64k-pes$WAKE_PES-h$h-r$round.txt" \
            OSOE_PROGRESS_HELPERS="$h" MULTISTREAM_STAT="$STAT" -- \
            "$KIT_BIN/multistream" put 65536 "$SECONDS_EACH"
        BESIDE="bash '$KIT/profile.sh' $PROFILE_SECONDS" BESIDE_DELAY=$PROFILE_DELAY \
            BESIDE_FILE="$OUT/profile-pes$WAKE_PES-h$h-r$round.txt" KIT_TIMEOUT=$WAKE_TIMEOUT \
            KIT_SALLOC="$KIT_SALLOC $(pair)" \
            beside 2 "$WAKE_PES" "$OUT/put-1m-profiled-pes$WAKE_PES-h$h-r$round.txt" \
            SHMEM_SYMMETRIC_SIZE="$HEAP" OSOE_PROGRESS_HELPERS="$h" -- \
            "$KIT_BIN/multistream" put 1048576 "$SECONDS_EACH"
        KIT_TIMEOUT=$WAKE_TIMEOUT collectives "$WAKE_PES" "$h" "$round"
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
