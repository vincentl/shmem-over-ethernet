#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# lead-ends.sh SECONDS NEVER PROGRAM [ARGS]...
#
# Rank NEVER exits before shmem_init; rank 0, its node's lead, runs PROGRAM and
# is killed SECONDS later, while it waits in the node's join for rank NEVER;
# every other rank runs PROGRAM. The others wait on the lead's count, print no
# notice of their own, and must end by themselves naming the lead (node.c,
# lead_ended): liveness starts only after the join.
SECONDS_TO_KILL=$1 NEVER=$2
shift 2
[ "$OSOE_PROCID" = "$NEVER" ] && exit 3
if [ "$OSOE_PROCID" = 0 ]; then
    "$@" &
    pid=$!
    sleep "$SECONDS_TO_KILL"
    kill -KILL "$pid"
    wait "$pid"
    exit 4
fi
exec "$@"
