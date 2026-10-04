#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A start by the same ids as one that failed takes none of what the failed one
# left ready (the release fixes' review, correctness F1): node 0's job tables
# and connection area, left by a start cancelled after node 0's lead made them,
# are replaced by the next start's lead, and the next start's PEs take only
# those. Before, a non-lead took the leftovers, and the start ended as a false
# deadlock, or hung with the detector off.
#
#   restart-leftovers.sh LAUNCHER STALLED PROGRAM [SHIM]
#
# STALLED (test-stalled-start.c) leaves them as start-leftovers.sh describes;
# PROGRAM (test-internode.c, or test-multinode.c under tsan) is then started by
# the same job id, with no osoe-stat --gc between, and must exit 0 and leave no
# name behind - a non-lead that took a leftover counted itself in the
# leftover's `opened`, and its own lead's object kept its name. With SHIM
# (slow-create.c), node 0's lead of the second start holds back making its
# connection area until its node-mate has taken the lead's job tables, whose
# name then goes, and half a second more, so that the node-mate meets the
# leftover area - which without the shim the lead has nearly always replaced
# first: the area's half of the fix, which the plain case does not reach (the
# review fixes' adversarial read). The shim says when it holds a creation, and
# a run with SHIM in which it did not fails. SECOND_MAP in the
# environment gives the second start another map, `0,0,1,1,2,2` say, so that
# the leftover area, made for two nodes, is smaller than this start's: a
# node-mate must take its size for a leftover's, not for its lead's settings.
set -u
LAUNCHER=$1 STALLED=$2 PROGRAM=$3 SHIM=${4:-}
J=$((920000000 + $$))
log=$(mktemp)
cleanup() { rm -f "$log" /dev/shm/osoe.$J.*; }
trap cleanup EXIT

"$LAUNCHER" -n 4 -m 0,0,1,1 -t 60 -e 0:OSOE_JOB_ID=$J -e 1:OSOE_JOB_ID=$J \
    -e 2:OSOE_JOB_ID=$J -e 3:OSOE_JOB_ID=$J -- "$STALLED" > "$log" 2>&1 &
LP=$!
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
sleep 1
kill -TERM $LP
wait $LP
left=$(ls /dev/shm | grep -c "^osoe\.$J\.0\.0\.\(job\|conn\)\.0$")
echo "node 0's objects the cancelled start left with a name: $left"

MAP=${SECOND_MAP:-0,0,1,1}
N=$(echo "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
set --
r=0
while [ $r -lt $N ]; do set -- "$@" -e $r:OSOE_JOB_ID=$J; r=$((r + 1)); done
[ -n "$SHIM" ] && set -- "$@" -e 0:LD_PRELOAD=$SHIM -e 0:OSOE_TEST_SLOW_NAME=.conn. \
    -e 0:OSOE_TEST_SLOW_AFTER=/dev/shm/osoe.$J.0.0.job.0
"$LAUNCHER" -n $N -m $MAP -t 60 "$@" -- "$PROGRAM" > "$log" 2>&1
status=$?
cat "$log"
echo "the start by the same ids exited $status"
after=$(ls /dev/shm | grep -c "^osoe\.$J\.")
echo "objects of job $J with a name afterwards: $after"

held=1
if [ -n "$SHIM" ] && ! grep -q "slow-create: holding the creation of " "$log"; then
    echo "the shim held no creation: it did not run, and the case was not reached"
    held=0
fi

[ "$left" -eq 2 ] && [ "$status" -eq 0 ] && [ "$after" -eq 0 ] && [ "$held" -eq 1 ] || exit 1
echo "RESTART LEFTOVERS OK"
