#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Data wires made on demand (H23; OSOE_MESH_DATA_WIRES=on-demand; data-wires.h),
# as each lead's SHMEM_DEBUG lines show them: "osoe: node A made its data wire
# toward node B on demand: HOW", HOW "it dialled", "the peer dialled, as asked"
# or "the peer dialled for its own need".
#
#   data-wires.sh LAUNCHER PROGRAM ring MAP [HELPERS]
#   data-wires.sh LAUNCHER PROGRAM skip MAP
#       test-data-wires.c's ring or skip over MAP, at HELPERS progress helpers
#       (0): the job right; every lead's mesh its control wires alone; a wire,
#       and its two ends, for every node pair the job's PEs talk across and
#       every edge of the collective tree, which shmem_malloc's check and the
#       link's hand-out at start-up use - node 0 and each other node, at the
#       default fan-out of 8 and nine nodes or fewer - and for no other pair;
#       every "it dialled" on the node the mesh's schedule has call the other,
#       and every "the peer dialled" on the other; under skip every wire of a
#       pair outside the tree one the asker asked for.
#   data-wires.sh LAUNCHER PROGRAM stall-caller
#   data-wires.sh LAUNCHER PROGRAM stall-callee
#       Two nodes, one held 4 s (OSOE_TEST_HOLD_DATA_WIRES) - node 0, the caller,
#       or node 1 - and the notice of 1 s (OSOE_TEST_NOTICE_SECONDS): the other
#       node's lead says what its data wire waits for, naming the node, its
#       address and, calling, the last call's error; then the wire is made and
#       the job ends right.
#   data-wires.sh LAUNCHER PROGRAM stall-long
#       Node 0 held 6 s, the deadlock detector's quiet at its least, 1 s
#       (OSOE_DEADLOCK_SECONDS): every PE waits in the library meanwhile, and a
#       node making a data wire is neither idle nor quiet, so the job is not
#       declared deadlocked - a stalled wire is told as any wait is - and ends
#       right once the wire is made.
#   data-wires.sh LAUNCHER PROGRAM stall-timeout
#       Node 1 held 30 s with OSOE_NO_PROGRESS_TIMEOUT=2: the job ends with a
#       failure, naming the data wire that is not made, well before the hold.
#   data-wires.sh LAUNCHER PROGRAM exit
#       Three nodes, node 0 holding back every data wire, and its PE calling
#       shmem_global_exit(3) while the others' first shmem_malloc waits for its
#       wire to node 0 - asked for by node 1, dialled by node 2: the caller
#       leaves with 3, every other rank with 0, nobody is killed or left
#       waiting, and nobody ends naming a lost goodbye (data_wire_model.py's
#       STATUS).
#
# PROGRAM is test-data-wires.
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
MAP=${4:-0,1}
HELPERS=${5:-0}
export OSOE_MESH_DATA_WIRES=on-demand
unset OSOE_TEST_HOLD_DATA_WIRES OSOE_NO_PROGRESS_TIMEOUT
# A hold begins at start-up, so the stalls' and the exit's link is set by hand:
# measured, it is a data wire of shmem_init's own (link-measure.h), and the
# hold would stall init itself rather than the program's first traffic.
case $MODE in
    stall-*|exit) export OSOE_LINK_HOP_NS=50000 OSOE_LINK_MESSAGE_NS=7000 \
                         OSOE_LINK_BYTES_PER_SECOND=1000000000 ;;
esac
case $MODE in
    ring|skip) ;;
    stall-caller) export OSOE_TEST_HOLD_DATA_WIRES=0:4000 OSOE_TEST_NOTICE_SECONDS=1 ;;
    stall-callee) export OSOE_TEST_HOLD_DATA_WIRES=1:4000 OSOE_TEST_NOTICE_SECONDS=1 ;;
    stall-long) export OSOE_TEST_HOLD_DATA_WIRES=0:6000 OSOE_DEADLOCK_SECONDS=1 ;;
    stall-timeout) export OSOE_TEST_HOLD_DATA_WIRES=1:30000 OSOE_NO_PROGRESS_TIMEOUT=2 ;;
    exit) MAP=0,1,2; export OSOE_TEST_HOLD_DATA_WIRES=0:20000 ;;
    *) echo "data-wires.sh: no mode '$MODE'"; exit 2 ;;
esac
ranks=$(printf '%s\n' "$MAP" | tr , '\n' | grep -c .)
program_mode=$MODE
case $MODE in stall-*) program_mode=ring ;; esac
began=$(date +%s)
out=$(SHMEM_DEBUG=1 OSOE_PROGRESS_HELPERS=$HELPERS "$LAUNCHER" -n "$ranks" -m "$MAP" -t 40 -k -- \
      "$PROGRAM" "$program_mode" 2>&1)
status=$?
took=$(( $(date +%s) - began ))
printf '%s\n' "$out"
echo "launcher exited $status after ${took} s"

# Whether node $1 calls node $2 in a job of $3 nodes: the mesh's schedule
# (mesh-schedule.h), the offsets 1 to (n - 1) / 2 round the circle, and an even
# n's opposite pairs from the lower node.
calls() {
    delta=$(( ($2 - $1 + $3) % $3 ))
    [ "$delta" -ge 1 ] && [ "$delta" -le $(( ($3 - 1) / 2 )) ] && return 0
    [ $(( $3 % 2 )) -eq 0 ] && [ "$delta" -eq $(( $3 / 2 )) ] && [ "$1" -lt "$2" ]
}
node_of() { printf '%s\n' "$MAP" | cut -d, -f$(( $1 + 1 )); }
nodes=$(printf '%s\n' "$MAP" | tr , '\n' | sort -u | grep -c .)
made=$(printf '%s\n' "$out" | grep '^osoe: node [0-9]* made its data wire toward node [0-9]* on demand: ')

case $MODE in
    ring|skip)
        [ "$status" -eq 0 ] || exit 1
        printf '%s\n' "$out" | grep -q '^DATA WIRES TEST OK$' || exit 1
        meshes=$(printf '%s\n' "$out" | grep -c '^osoe: node [0-9]* connected its mesh in .*; its data wires on demand$')
        echo "$meshes of $nodes meshes their control wires alone"
        [ "$meshes" -eq "$nodes" ] || exit 1
        step=1
        [ "$MODE" = skip ] && step=3
        # The node pairs the PEs talk across, each "a-b" with a < b, and the
        # collective tree's edges.
        talk=$(pe=0; while [ "$pe" -lt "$ranks" ]; do
            a=$(node_of "$pe"); b=$(node_of $(( (pe + step) % ranks )))
            if [ "$a" != "$b" ]; then
                [ "$a" -lt "$b" ] && echo "$a-$b" || echo "$b-$a"
            fi
            pe=$((pe + 1))
        done | sort -u)
        tree=$(seq 1 $((nodes - 1)) | sed 's/^/0-/')
        pairs=$(printf '%s\n' "$talk" "$tree" | grep . | sort -u)
        wanted=$(printf '%s\n' "$pairs" | grep -c .)
        lines=$(printf '%s\n' "$made" | grep -c .)
        echo "$wanted node pairs of $(( nodes * (nodes - 1) / 2 )) talk or are the tree's; $lines ends made"
        [ "$lines" -eq $(( 2 * wanted )) ] || exit 1
        for pair in $pairs; do
            a=${pair%-*}; b=${pair#*-}
            if calls "$a" "$b" "$nodes"; then caller=$a callee=$b; else caller=$b callee=$a; fi
            printf '%s\n' "$made" | grep -q "^osoe: node $caller made its data wire toward node $callee on demand: it dialled" ||
                { echo "node $caller, the caller of $pair, did not dial it"; exit 1; }
            printf '%s\n' "$made" | grep -q "^osoe: node $callee made its data wire toward node $caller on demand: the peer dialled" ||
                { echo "node $callee, the callee of $pair, did not answer it"; exit 1; }
            if [ "$MODE" = skip ] && [ "$nodes" -eq "$ranks" ] &&
               ! printf '%s\n' "$tree" | grep -qx "$pair"; then
                printf '%s\n' "$made" | grep -q "^osoe: node $callee made its data wire toward node $caller on demand: the peer dialled, as asked" ||
                    { echo "node $callee did not ask for $pair"; exit 1; }
            fi
            echo "$pair: node $caller dialled"
        done ;;
    stall-long)
        [ "$status" -eq 0 ] || exit 1
        printf '%s\n' "$out" | grep -q '^DATA WIRES TEST OK$' || exit 1
        printf '%s\n' "$out" | grep -qi 'deadlock' && { echo "declared deadlocked"; exit 1; }
        [ "$took" -ge 5 ] || { echo "the hold did not hold"; exit 1; } ;;
    stall-caller|stall-callee)
        [ "$status" -eq 0 ] || exit 1
        printf '%s\n' "$out" | grep -q '^DATA WIRES TEST OK$' || exit 1
        if [ "$MODE" = stall-caller ]; then
            pattern='node 1 has waited [0-9]+ s for node 0 \(127\.0\.0\.1, at 127\.0\.0\.1\) to dial their data wire, which it asked for on their control wire; still waiting'
        else
            pattern='node 0 has waited [0-9]+ s to make its data wire toward node 1, for node 1 \(127\.0\.0\.2, at 127\.0\.0\.2\) to answer on port [0-9]+: [^;]+, [0-9.]+ s ago; still waiting'
        fi
        printf '%s\n' "$out" | grep -Eq "$pattern" || { echo "no notice: $pattern"; exit 1; } ;;
    stall-timeout)
        [ "$status" -ne 0 ] || exit 1
        printf '%s\n' "$out" | grep -q 'killed by signal\|NOT TERMINATED\|timeout after' && exit 1
        printf '%s\n' "$out" | grep -Eq "OSOE_NO_PROGRESS_TIMEOUT\\): (node 0's data wire toward node 1 is not made|the turnover of a connection whose data wire is not yet made has stayed at 0)" ||
            { echo "no fatal naming the data wire"; exit 1; } ;;
    exit)
        printf '%s\n' "$out" | grep -q 'killed by signal\|NOT TERMINATED\|timeout after\|without its goodbye' && exit 1
        [ "$status" -eq 3 ] || exit 1
        printf '%s\n' "$out" | grep -q "rank 0 exited with status 3\$" || exit 1
        others=$(printf '%s\n' "$out" | grep -c "exited with status [0-9]*\$")
        echo "$others ranks left nonzero"
        [ "$others" -eq 1 ] || exit 1
        # Ended by the caller, well within the abort's grace of 10 s - nothing
        # waits for a wire - and not by the hold's end (global-exit.sh's 6 s).
        [ "$took" -lt 6 ] || exit 1
        printf '%s\n' "$made" | grep -q . && { echo "a data wire was made before the end"; exit 1; }
        printf '%s\n' "$out" | grep -q "returned though PE 0 ends the job" && exit 1 ;;
esac
echo "DATA WIRES OK"
