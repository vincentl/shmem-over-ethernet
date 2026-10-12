#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# OSU OMB 7.5.2 on the cluster (2026-10-12), after the recipe in docs/pi-cluster.md:
# point-to-point between two nodes, then the collectives at 8 nodes x 16 PEs, each
# run under a timeout so that none can hold the session.
set -u
O=~/out/osu; mkdir -p "$O"; B=$HOME/osu/c/openshmem
for p in put get put_bw get_bw atomics; do
    echo "== $p $(date -u +%H:%M:%S)" >> "$O/run.log"
    timeout 300 srun -K1 -N2 --ntasks-per-node=1 --chdir="$B" ./osu_oshm_$p heap > "$O/osu-$p.txt" 2>&1
    echo "exit $?" >> "$O/run.log"
done
for p in barrier broadcast collect fcollect reduce; do
    echo "== $p 8x16 $(date -u +%H:%M:%S)" >> "$O/run.log"
    timeout 600 srun -K1 -N8 --ntasks-per-node=16 --chdir="$B" ./osu_oshm_$p -m :1048576 > "$O/osu-$p-8x16.txt" 2>&1
    echo "exit $?" >> "$O/run.log"
done
echo "DONE $(date -u +%H:%M:%S)" >> "$O/run.log"
