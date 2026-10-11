#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# lead-ends.sh with one change: rank LATE starts PROGRAM only DELAY seconds
# in, after the lead has been killed - a node-mate that arrives after its lead
# has died, which tier 2's load on a Pi node made of shmem.start-up.lead-ends-in-join.
# lead-ends-late.sh SECONDS NEVER LATE DELAY PROGRAM [ARGS]...
SECONDS_TO_KILL=$1 NEVER=$2 LATE=$3 DELAY=$4
shift 4
[ "$OSOE_PROCID" = "$NEVER" ] && exit 3
if [ "$OSOE_PROCID" = 0 ]; then
    "$@" &
    pid=$!
    sleep "$SECONDS_TO_KILL"
    kill -KILL "$pid"
    wait "$pid"
    exit 4
fi
[ "$OSOE_PROCID" = "$LATE" ] && sleep "$DELAY"
exec "$@"
