#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# start-after-lead-ends.sh LAUNCHER SHIM PROGRAM [no-launch]
#
# A start by the ids of one whose lead ended before it stored `ready` runs (the
# release fixes' review, correctness F2). The first start is one PE, its lead
# ended by SIGKILL as it creates its heap (kill-on-create.c); it leaves the
# node's control segment sized and never ready, with the pid of a process that
# is gone in it. The second start, three PEs by the same ids, holds rank 0 back
# two seconds, so that its node-mates open that leftover while it stands: they
# must wait past it for their lead to reclaim it, and not end, naming a lead
# that ended - which it is, but another start's (node.c, open_control and
# lead_ended_before_ready; lead-ends-before-ready.sh is the lead that is this
# start's).
#
# What tells the leftover from the start's own segment is the launch number in
# its header, which osoe-launch gives each start at random. With no-launch both
# starts are given 0, as a first Slurm run is, which tells nothing: the check is
# off, and the node-mates wait as they did. Either way the second start must
# run, reclaim the leftover and leave no name behind.
set -u
LAUNCHER=$1 SHIM=$2 PROGRAM=$3 MODE=${4:-}
J=$((950000000 + $$ % 1000000))
log=$(mktemp)
cleanup() { rm -f "$log" /dev/shm/osoe.$J.*; }
trap cleanup EXIT

# The launcher's -e options for $1 ranks: the job id, the same for every start,
# and the launch number 0 where asked.
ids() {
    r=0
    while [ "$r" -lt "$1" ]; do
        printf ' -e %s:OSOE_JOB_ID=%s' "$r" "$J"
        [ "$MODE" = no-launch ] && printf ' -e %s:OSOE_LAUNCH=0' "$r"
        r=$((r + 1))
    done
}

failed=0
# shellcheck disable=SC2046 # ids is a list of launcher arguments
"$LAUNCHER" -n 1 -t 20 -k $(ids 1) -e 0:LD_PRELOAD="$SHIM" -e 0:OSOE_TEST_KILL_NAME=.heap. \
    -- "$PROGRAM" > "$log" 2>&1
if ! grep -q "kill-on-create: ending this process at its creation of /osoe.$J.0.0.0.heap.0" "$log"; then
    cat "$log"
    echo "the first start's lead was not ended at its heap"
    failed=1
fi
if [ ! -e "/dev/shm/osoe.$J.0.0.0" ]; then
    echo "the first start left no control segment"
    failed=1
fi

# shellcheck disable=SC2046
out=$("$LAUNCHER" -n 3 -t 30 $(ids 3) \
    -- sh -c '[ "$OSOE_PROCID" = 0 ] && sleep 2; exec "$0"' "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "the second start exited $status"
[ "$status" -eq 0 ] || failed=1
printf '%s\n' "$out" | grep -q "osoe: reclaimed /dev/shm/osoe.$J.0.0.0, " || {
    echo "the second start's lead reclaimed nothing: the leftover was not there"
    failed=1
}
if printf '%s\n' "$out" | grep -q "has ended before finishing"; then
    echo "a node-mate took the first start's leftover for its own lead's segment"
    failed=1
fi
after=$(ls /dev/shm | grep -c "^osoe\.$J\.")
echo "objects of job $J with a name afterwards: $after"
[ "$after" -eq 0 ] || failed=1

[ "$failed" -eq 0 ] && echo "START AFTER LEAD ENDS OK"
exit "$failed"
