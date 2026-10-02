#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# osoe-stat --gc removes the names a start that never finished leaves in
# /dev/shm - a node's job tables and connection area (the round-three review's
# R5a, which found them under srun) - and only once nothing uses them.
#
#   start-leftovers.sh LAUNCHER PROGRAM STAT
#
# PROGRAM (test-stalled-start.c) is a four-PE job on two nodes in which one PE of
# node 0 stops inside shmem_init after its node has joined, so that node 0's
# lead makes the node's job tables and connection area, opens each, and waits
# for the PE that will not: both names stay. The job runs under a fixed job id,
# which the launcher does not know, so that its own removal of leftovers - which
# hides them under srun's absence - does not reach them (leftovers.sh does the
# same). Then:
#
#   1. --gc while the start is stuck, every PE alive: both objects are left
#      alone, and are still there.
#   2. The start is cancelled, as scancel would: SIGTERM to the launcher, which
#      forwards it and exits once every PE is gone.
#   3. --gc again: both are removed, and nothing of the job has a name.
set -u
LAUNCHER=$1 PROGRAM=$2 STAT=$3
J=$((910000000 + $$))
log=$(mktemp)
cleanup() { rm -f "$log" /dev/shm/osoe.$J.*; }
trap cleanup EXIT

"$LAUNCHER" -n 4 -m 0,0,1,1 -t 60 -e 0:OSOE_JOB_ID=$J -e 1:OSOE_JOB_ID=$J \
    -e 2:OSOE_JOB_ID=$J -e 3:OSOE_JOB_ID=$J -- "$PROGRAM" > "$log" 2>&1 &
LP=$!

# Node 1 starts 8 s late, and node 0's lead makes both objects at once when it
# does; wait for the second to be there.
i=0
while [ ! -e /dev/shm/osoe.$J.0.0.job.0 ] || [ ! -e /dev/shm/osoe.$J.0.0.conn.0 ]; do
    i=$((i + 1))
    if [ $i -gt 400 ]; then
        cat "$log"
        echo "the start never made node 0's job tables and connection area"
        kill -TERM $LP; wait $LP
        exit 1
    fi
    sleep 0.1
done
# Node 1 makes and opens its own two objects in the same moments, and they lose
# their names as it does; a second is more than they take.
sleep 1
named=$(ls /dev/shm | grep -c "^osoe\.$J\.0\.0\.")
echo "objects of job $J's node 0 with a name while its start is stuck: $named"

echo "--- osoe-stat --gc, every PE alive"
stuck=$("$STAT" --gc 2>&1)
printf '%s\n' "$stuck"
alone=$(printf '%s\n' "$stuck" | grep -c "^/osoe\.$J\.0\.0\.\(job\|conn\)\.0: .*left alone")
echo "left alone while its start was stuck: $alone"
still=$(ls /dev/shm | grep -c "^osoe\.$J\.0\.0\.")
echo "objects of job $J's node 0 with a name after that: $still"

echo "--- the start is cancelled"
kill -TERM $LP
wait $LP
echo "launcher exited $?"

echo "--- osoe-stat --gc, every PE gone"
gone=$("$STAT" --gc 2>&1)
printf '%s\n' "$gone"
removed=$(printf '%s\n' "$gone" | grep -c "^/osoe\.$J\.0\.0\.\(job\|conn\)\.0: removed")
echo "removed: $removed"
left=$(ls /dev/shm | grep -c "^osoe\.$J\.")
echo "objects of job $J with a name afterwards: $left"

[ "$named" -eq 2 ] && [ "$alone" -eq 2 ] && [ "$still" -eq 2 ] &&
    [ "$removed" -eq 2 ] && [ "$left" -eq 0 ] || exit 1
echo "START LEFTOVERS OK"
