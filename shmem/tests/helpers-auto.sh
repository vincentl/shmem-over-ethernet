#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# OSOE_PROGRESS_HELPERS=auto (P36; bootstrap.h's OSOE_PROGRESS_HELPERS_AUTO):
# two progress helpers on a job of three nodes or more where a node runs no
# more PEs than it has CPUs online, none otherwise - as each node's lead says
# it under SHMEM_DEBUG, from the count its connection area was laid out for.
# OSOE_TEST_ONLINE_CPUS stands in for the machine's CPUs, so that the cases
# hold on any machine.
#
#   helpers-auto.sh LAUNCHER PROGRAM three|two|crowded
#
#   three    0,1,2 on 4 CPUs: every node runs two.
#   two      0,0,1,1 on 4 CPUs: a job of two nodes, so none on either.
#   crowded  0,0,0,1,2 on 2 CPUs: node 0's three PEs outnumber its CPUs, so
#            it runs none, and nodes 1 and 2, one PE each, two - nodes that
#            differ, as they may.
#
# PROGRAM moves data between every pair of PEs, through the helpers where
# there are some, and checks it.
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
case $MODE in
three)   MAP=0,1,2     CPUS=4 ;;
two)     MAP=0,0,1,1   CPUS=4 ;;
crowded) MAP=0,0,0,1,2 CPUS=2 ;;
*)       echo "helpers-auto.sh: no case '$MODE'"; exit 2 ;;
esac
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
nodes=$(printf '%s\n' "$MAP" | tr ',' '\n' | sort -u | wc -l | tr -d ' ')
out=$(SHMEM_DEBUG=1 OSOE_PROGRESS_HELPERS=auto OSOE_TEST_ONLINE_CPUS=$CPUS \
    "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
said='^osoe: node [0-9]* runs [0-9]* progress helpers (OSOE_PROGRESS_HELPERS=auto): '
lines=$(printf '%s\n' "$out" | grep -c "$said")
echo "nodes saying their count: $lines of $nodes"
[ "$lines" -eq "$nodes" ] || exit 1
# expect NODE HELPERS PES REASON: node NODE's line, exactly.
expect() {
    line="osoe: node $1 runs $2 progress helpers (OSOE_PROGRESS_HELPERS=auto): the job's nodes"
    line="$line $nodes, this node's PEs $3 and CPUs online $CPUS$4"
    printf '%s\n' "$out" | grep -qxF "$line" || { echo "missing: $line"; exit 1; }
}
case $MODE in
three)
    for node in 0 1 2; do expect $node 2 1 ""; done ;;
two)
    for node in 0 1; do expect $node 0 2 " - two need three nodes or more"; done ;;
crowded)
    expect 0 0 3 " - two need no more PEs than CPUs"
    for node in 1 2; do expect $node 2 1 ""; done ;;
esac
echo "HELPERS AUTO OK"
