#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A job that asks for more /dev/shm than there is is refused at init, by name,
# instead of dying of SIGBUS later (§6.2; review round two, omissions 5).
#
#   dev-shm-capacity.sh LAUNCHER PROGRAM heaps|connections|warned|node-ends|broken-pipe
#
# Sizes the request from what is free now, so it exceeds this machine's
# /dev/shm whatever that is:
#   heaps        two PEs on one node, each asking for a heap as large as all of
#                /dev/shm's free space.
#   connections  enough nodes of 1 GiB connection buffers that a node's
#                connection area (two buffers per peer) exceeds the free space,
#                with a heap small enough to fit.
#   warned       heaps, with OSOE_SHM_CAPACITY_CHECK=0: the shortfall is
#                reported and the job runs, since it touches almost none of
#                the heap it asked for.
#   node-ends    heaps, and both PEs must end by themselves: the lead naming
#                the shortfall, the other "another local PE failed during
#                rendezvous", neither killed by the launcher (-k, refusal.sh).
#                The lead checks the room once the control segment exists, so
#                that there is a `failed` word to refuse through; checked
#                before, its node-mate waited for a segment that never came.
#                The segment's name stays, failed, as any failed job's does.
#   broken-pipe  heaps, with the launcher's output in a pipe whose reader has
#                gone, as `osoe-launch ... | head` leaves it: the launcher
#                must outlive its own lines and remove the segment the refused
#                start left, and the PEs leave with the refusal's status, not
#                by SIGPIPE - so the launcher exits 1, not 141. Killed by
#                SIGPIPE, the launcher left the segment in /dev/shm (the v0.2
#                step 1 review).
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
free=$(df --output=avail -B1 /dev/shm | tail -1 | tr -d ' ')
echo "free in /dev/shm: $free bytes"
if [ "${free:-0}" -le 0 ]; then
    echo "no free space in /dev/shm to size a request from"
    exit 1
fi
case $MODE in
heaps)
    "$LAUNCHER" -n 2 -t 30 -e 0:SHMEM_SYMMETRIC_SIZE=$free \
        -e 1:SHMEM_SYMMETRIC_SIZE=$free -- "$PROGRAM"
    ;;
warned)
    OSOE_SHM_CAPACITY_CHECK=0 "$LAUNCHER" -n 2 -t 30 -e 0:SHMEM_SYMMETRIC_SIZE=$free \
        -e 1:SHMEM_SYMMETRIC_SIZE=$free -- "$PROGRAM"
    ;;
node-ends)
    exec sh "$(dirname "$0")/refusal.sh" "$LAUNCHER" 2 0 \
        "osoe: PE 0: /dev/shm has [0-9]+ MiB free, but this node's control segment and 2 heaps" \
        "osoe: PE 1: another local PE failed during rendezvous" \
        "osoe-launch: left behind: /dev/shm/osoe[.][0-9]+[.]0[.]0[.]0\$" \
        -- -n 2 -t 8 -e 0:SHMEM_SYMMETRIC_SIZE=$free -e 1:SHMEM_SYMMETRIC_SIZE=$free \
        -- "$PROGRAM"
    ;;
broken-pipe)
    dir=$(mktemp -d)
    {
        # Write until the reader, true, is gone - each write fails with EPIPE
        # once it is, SIGPIPE ignored meanwhile - then start the launcher
        # with SIGPIPE back at its default, into the broken pipe.
        trap '' PIPE
        while env printf x 2>/dev/null; do sleep 0.01; done
        trap - PIPE
        "$LAUNCHER" -n 2 -t 30 -e 0:SHMEM_SYMMETRIC_SIZE=$free \
            -e 1:SHMEM_SYMMETRIC_SIZE=$free -- "$PROGRAM" &
        echo $! > "$dir/pid"
        wait $!
        echo $? > "$dir/status"
    } 2>&1 | true
    pid=$(cat "$dir/pid") status=$(cat "$dir/status")
    rm -rf "$dir"
    left=$(ls /dev/shm | grep "^osoe[.]$pid[.]" || true)
    echo "launcher $pid exited $status; left in /dev/shm: ${left:-nothing}"
    for name in $left; do rm -f "/dev/shm/$name"; done
    [ -z "$left" ] && [ "$status" = 1 ] && echo "BROKEN PIPE OK"
    exit 0
    ;;
connections)
    gib=1073741824
    nodes=$((free / (2 * gib) + 2))
    # 1 GiB is the largest buffer, and 2 GiB a peer against a 64 GiB budget is
    # 33 nodes: a /dev/shm with more free than that is refused for the budget,
    # not for the space, so there is nothing here to test.
    if [ $nodes -gt 33 ]; then
        echo "skipped: $free bytes free is more than a connection area can ask for"
        exit 77
    fi
    map=0; i=1
    while [ $i -lt $nodes ]; do map="$map,$i"; i=$((i + 1)); done
    OSOE_AM_BUFFER=$gib OSOE_AM_BUFFER_BUDGET=$((64 * gib)) SHMEM_SYMMETRIC_SIZE=1m \
        "$LAUNCHER" -n $nodes -m $map -t 30 -- "$PROGRAM"
    ;;
esac
echo "launcher exited $?"
