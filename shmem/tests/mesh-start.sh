#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The mesh's start-up schedule as each lead's SHMEM_DEBUG line shows it (H23;
# wire.c: "osoe: node N connected its mesh in X us: A wires answered and C
# called, in S slots of D us, W calls in flight at most").
#
#   mesh-start.sh LAUNCHER PROGRAM hold    seven nodes, node 3 reaching its mesh
#                                          1.5 s late (OSOE_TEST_MESH_HOLD): the
#                                          nodes that call it - 0, 1 and 2 - wait
#                                          for it, their mesh 1.4 s or more,
#                                          while node 3's own, counted from its
#                                          arrival, is shorter
#   mesh-start.sh LAUNCHER PROGRAM slot    nine nodes at OSOE_MESH_SLOT=20000:
#                                          four slots of 20 ms, and every lead
#                                          calls in each, so no lead's mesh is
#                                          shorter than its last slot's 60 ms
#   mesh-start.sh LAUNCHER PROGRAM none    eight nodes at OSOE_MESH_SLOT=0, each
#                                          call as soon as the window has room
#   mesh-start.sh LAUNCHER PROGRAM crowd   21 nodes at OSOE_MESH_SLOT=1: twenty
#                                          calls a lead, every slot due at once,
#                                          twelve waiting for a place in the
#                                          window long after their slot - made
#                                          late, never skipped, or the mesh
#                                          never completes (mesh_model.py's
#                                          skip_late)
#
# PROGRAM is any program that initializes, once, and finishes.
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
case $MODE in
    hold) nodes=7; export OSOE_TEST_MESH_HOLD=3:1500; unset OSOE_MESH_SLOT ;;
    slot) nodes=9; export OSOE_MESH_SLOT=20000; unset OSOE_TEST_MESH_HOLD ;;
    none) nodes=8; export OSOE_MESH_SLOT=0; unset OSOE_TEST_MESH_HOLD ;;
    crowd) nodes=21; export OSOE_MESH_SLOT=1; unset OSOE_TEST_MESH_HOLD ;;
    *)    echo "mesh-start.sh: no mode '$MODE' (hold, slot, none, crowd)"; exit 2 ;;
esac
map=$(seq -s, 0 $((nodes - 1)))
out=$(SHMEM_DEBUG=1 "$LAUNCHER" -n "$nodes" -m "$map" -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
lines=$(printf '%s\n' "$out" | grep '^osoe: node [0-9]* connected its mesh in ')
count=$(printf '%s\n' "$lines" | grep -c .)
[ "$count" -eq "$nodes" ] || { echo "$count mesh lines for $nodes nodes"; exit 1; }
us() {
    printf '%s\n' "$lines" | sed -n "s/^osoe: node $1 connected its mesh in \([0-9]*\) us.*/\1/p"
}
case $MODE in
    hold)
        held=$(us 3)
        echo "node 3, held: ${held} us from its arrival"
        [ "$held" -lt 1400000 ] || exit 1
        for caller in 0 1 2; do
            waited=$(us $caller)
            echo "node $caller, which calls node 3: ${waited} us"
            [ "$waited" -ge 1400000 ] || exit 1
        done ;;
    slot|none|crowd)
        slot=$OSOE_MESH_SLOT
        slots=$((nodes / 2))
        least=$(( (slots - 1) * slot ))
        node=0
        while [ "$node" -lt "$nodes" ]; do
            line=$(printf '%s\n' "$lines" | grep "^osoe: node $node connected")
            took=$(us $node)
            echo "node $node: ${took} us, ${least} us at least"
            printf '%s\n' "$line" | grep -q "in $slots slots of $slot us" || exit 1
            [ "$took" -ge "$least" ] || exit 1
            node=$((node + 1))
        done ;;
esac
echo "MESH START OK"
