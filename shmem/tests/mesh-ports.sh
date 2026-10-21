#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# OSOE_MESH_PORT_RANGE (P18): each node's lead listens for its mesh on a free
# port of the range, as its SHMEM_DEBUG line says, and the job runs.
#
#   mesh-ports.sh LAUNCHER PROGRAM MAP LOW HIGH
#
# PROGRAM is any program that initializes, once, and finishes.
set -u
LAUNCHER=$1 PROGRAM=$2 MAP=$3 LOW=$4 HIGH=$5
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
nodes=$(printf '%s\n' "$MAP" | tr ',' '\n' | sort -u | wc -l | tr -d ' ')
out=$(SHMEM_DEBUG=1 OSOE_MESH_PORT_RANGE=$LOW-$HIGH \
    "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
ports=$(printf '%s\n' "$out" |
        sed -n "s/^osoe: node [0-9]* listens for its mesh on port \([0-9]*\) (OSOE_MESH_PORT_RANGE=$LOW-$HIGH)\$/\1/p")
said=$(printf '%s\n' "$ports" | grep -c .)
echo "leads saying their mesh's port: $said of $nodes"
[ "$said" -eq "$nodes" ] || exit 1
for port in $ports; do
    [ "$port" -ge "$LOW" ] && [ "$port" -le "$HIGH" ] || { echo "port $port is outside $LOW-$HIGH"; exit 1; }
done
distinct=$(printf '%s\n' "$ports" | sort -u | grep -c .)
[ "$distinct" -eq "$nodes" ] || { echo "$distinct distinct ports for $nodes leads"; exit 1; }
echo "MESH PORTS OK"
