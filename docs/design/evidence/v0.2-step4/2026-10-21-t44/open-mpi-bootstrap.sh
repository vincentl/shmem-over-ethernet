#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: osoe's bootstrap variables, made in each rank from the ones Open MPI's
# mpirun sets there, for a job on one machine (README, "Starting PEs without a
# launcher").
#
#   [T44_NODES=K] mpirun -n N [options] open-mpi-bootstrap.sh PROGRAM [ARGS]...
#
# One node, or K logical nodes of N / K ranks each, rank r on node r / (N / K),
# each node's lead on its own loopback address, 127.0.0.<node + 1>, as
# osoe-launch gives them (design 8.1). N must be a multiple of K.
#
# What Open MPI gives and osoe needs:
#   OMPI_COMM_WORLD_RANK  -> OSOE_PROCID
#   OMPI_COMM_WORLD_SIZE  -> OSOE_STEP_NUM_TASKS
#   the node, its ranks   -> OSOE_NODEID, OSOE_STEP_NUM_NODES, OSOE_LOCALID, OSOE_GTIDS
#   the job's identity    -> OSOE_JOB_ID, OSOE_STEP_ID, OSOE_LAUNCH
# On one real node OMPI_COMM_WORLD_LOCAL_RANK and OMPI_COMM_WORLD_LOCAL_SIZE
# say the same as the arithmetic below (checked, when K is 1). Several machines
# would need each node's number and its ranks: Open MPI's variables carry
# neither, so a wrapper would need a host list of its own. Not tried.
set -eu

fail() {
    echo "open-mpi-bootstrap: $*" >&2
    exit 2
}

[ $# -ge 1 ] || fail "usage: open-mpi-bootstrap.sh PROGRAM [ARGS]..."
rank=${OMPI_COMM_WORLD_RANK:-}
size=${OMPI_COMM_WORLD_SIZE:-}
[ -n "$rank" ] && [ -n "$size" ] || fail "not started by mpirun: OMPI_COMM_WORLD_RANK is not set"
nodes=${T44_NODES:-1}
[ $((size % nodes)) -eq 0 ] || fail "$size ranks do not divide into $nodes logical nodes"
per_node=$((size / nodes))
node=$((rank / per_node))
first=$((node * per_node))
local_id=$((rank - first))
if [ "$nodes" -eq 1 ] && [ -n "${OMPI_COMM_WORLD_LOCAL_RANK:-}" ]; then
    [ "$OMPI_COMM_WORLD_LOCAL_RANK" -eq "$local_id" ] && [ "$OMPI_COMM_WORLD_LOCAL_SIZE" -eq "$per_node" ] ||
        fail "mpirun placed this rank on more than one node: local rank $OMPI_COMM_WORLD_LOCAL_RANK of $OMPI_COMM_WORLD_LOCAL_SIZE"
fi
gtids=
i=0
while [ $i -lt "$per_node" ]; do
    gtids="$gtids${gtids:+,}$((first + i))"
    i=$((i + 1))
done

# A number unique among the jobs on this machine at once: the pid of the
# mpirun that started the job, which is this process's parent when the job is
# on one node. And the start, from that pid's start time in clock ticks since
# boot, the same in every rank of the job and different for a later job that
# has the pid again.
job=$PPID
launch=$(awk '{ print $22 }' "/proc/$PPID/stat")

OSOE_PROCID=$rank
OSOE_STEP_NUM_TASKS=$size
OSOE_NODEID=$node
OSOE_STEP_NUM_NODES=$nodes
OSOE_LOCALID=$local_id
OSOE_GTIDS=$gtids
OSOE_JOB_ID=$job
OSOE_STEP_ID=0
OSOE_LAUNCH=$launch
if [ "$nodes" -gt 1 ]; then
    OSOE_STEP_NODELIST="127.0.0.[1-$nodes]"
else
    OSOE_STEP_NODELIST=$(hostname)
fi
export OSOE_PROCID OSOE_STEP_NUM_TASKS OSOE_NODEID OSOE_STEP_NUM_NODES OSOE_LOCALID \
    OSOE_GTIDS OSOE_JOB_ID OSOE_STEP_ID OSOE_LAUNCH OSOE_STEP_NODELIST
exec "$@"
