#!/bin/sh
# A job that asks for more /dev/shm than there is is refused at init, by name,
# instead of dying of SIGBUS later (§6.2; review round two, omissions 5).
#
#   dev-shm-capacity.sh LAUNCHER PROGRAM heaps|connections|warned
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
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
free=$(df --output=avail -B1 /dev/shm | tail -1 | tr -d ' ')
echo "free in /dev/shm: $free bytes"
case $MODE in
heaps)
    "$LAUNCHER" -n 2 -t 30 -e 0:SHMEM_SYMMETRIC_SIZE=$free \
        -e 1:SHMEM_SYMMETRIC_SIZE=$free -- "$PROGRAM"
    ;;
warned)
    OSOE_SHM_CAPACITY_CHECK=0 "$LAUNCHER" -n 2 -t 30 -e 0:SHMEM_SYMMETRIC_SIZE=$free \
        -e 1:SHMEM_SYMMETRIC_SIZE=$free -- "$PROGRAM"
    ;;
connections)
    gib=1073741824
    nodes=$((free / (2 * gib) + 2))
    map=0; i=1
    while [ $i -lt $nodes ]; do map="$map,$i"; i=$((i + 1)); done
    OSOE_AM_BUFFER=$gib OSOE_AM_BUFFER_BUDGET=$((64 * gib)) SHMEM_SYMMETRIC_SIZE=1m \
        "$LAUNCHER" -n $nodes -m $map -t 30 -- "$PROGRAM"
    ;;
esac
echo "launcher exited $?"
