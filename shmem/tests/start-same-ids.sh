#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# start-same-ids.sh LAUNCHER PROGRAM
#
# Five times: a start whose lead refuses its environment leaves a tombstone
# under the node's control segment's name, and a start by the same ids - job,
# step, node, session - must then run. A requeued Slurm job without the
# epilog's cleanup is such a start, and a launcher's reused pid another. Its
# PEs take the tombstone for another launch's leftover, by the launch the
# header records, and wait for their lead to reclaim it (node.c, open_control):
# before, a node-mate that read it first ended with "another local PE failed",
# one start in four (the adversarial read of 2026-10-03).
LAUNCHER=$1 PROGRAM=$2
J=$((940000000 + $$ % 1000000))
ids="-e 0:OSOE_JOB_ID=$J -e 1:OSOE_JOB_ID=$J -e 2:OSOE_JOB_ID=$J"
failed=0
for i in 1 2 3 4 5; do
    # shellcheck disable=SC2086 # ids is a list of launcher arguments
    "$LAUNCHER" -n 3 -t 20 -k $ids -e 0:OSOE_MAX_THREADS=0 -- "$PROGRAM" > /dev/null 2>&1
    if [ ! -e "/dev/shm/osoe.$J.0.0.0" ]; then
        echo "run $i: the refusal left no tombstone"
        failed=1
    fi
    # shellcheck disable=SC2086
    out=$("$LAUNCHER" -n 3 -t 20 $ids -- "$PROGRAM" 2>&1)
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "run $i: the start by the same ids failed, status $status:"
        printf '%s\n' "$out"
        failed=1
    fi
done
rm -f /dev/shm/osoe.$J.*
[ "$failed" -eq 0 ] && echo "SAME IDS OK"
