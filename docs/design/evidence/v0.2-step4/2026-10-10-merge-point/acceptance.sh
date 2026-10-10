#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4's merge point: the release checklist's acceptance past 61 PEs at
# OSOE_PROGRESS_HELPERS=H, on a release build at the default OSOE_MAX_TEAMS (256):
# the SOS team and active-set programs at 70 PEs on one node and on ten logical
# nodes of seven, collect_active_set at 32 on one node and on four of eight (its
# own limit), and the per-node split of SHMEM_TEAM_SHARED at 61, 62 and 64 nodes -
# the release point's acceptance.sh, with the helper count.
# usage: acceptance.sh TAR COMMIT OUTDIR H. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
C=<repo>/corpus/tests-sos/test/unit
S=<repo>/build/scratch/review-r3/scaling/split_shared.c
TAR=$1; COMMIT=$2; D=$3; H=$4; mkdir -p "$D"
export OSOE_PROGRESS_HELPERS=$H
OUT=$D/results-h$H.txt; : > "$OUT"
rm -rf /tmp/acc; mkdir -p /tmp/acc/bin
tar -C /tmp/acc -xf "${TAR:?archive}"
cd /tmp/acc && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > /dev/null 2>&1 || { echo "build failed" >> "$OUT"; exit 1; }
echo "built ${COMMIT:?commit} at OSOE_PROGRESS_HELPERS=$H $(date +%H:%M:%S)" >> "$OUT"
O=/tmp/acc/build/release/tools/wrapper/oshcc
L=/tmp/acc/build/release/tools/launch/osoe-launch
cd /tmp/acc/bin
PROGRAMS="shmem_team_reuse_teams reduce_active_set broadcast_active_set collect_active_set repeated_barriers shmem_team_max shmem_team_split_2d"
for t in $PROGRAMS; do
  $O -O1 -w -o $t $C/$t.c 2> $t.build || { echo "$t: build failed" >> "$OUT"; continue; }
done
$O -O1 -o split_shared $S
BLOCK10=$(for n in $(seq 0 9); do for k in 1 2 3 4 5 6 7; do printf "%d," $n; done; done | sed 's/,$//')
FOUR8=$(for n in 0 1 2 3; do for k in 1 2 3 4 5 6 7 8; do printf "%d," $n; done; done | sed 's/,$//')
one() {   # program label PEs map-arguments...
  t=$1 label=$2 n=$3; shift 3
  SHMEM_SYMMETRIC_SIZE=4m OSOE_SHM_CAPACITY_CHECK=0 $L -t 300 -n $n "$@" -- ./$t > "$D/$t.$label.h$H.out" 2> "$D/$t.$label.h$H.err"; rc=$?
  echo "$t $label: rc=$rc no-number=$(grep -c 'no team number is free\|no two team numbers are free' "$D/$t.$label.h$H.err") dry=$(grep -c 'has no team block left' "$D/$t.$label.h$H.err") rank-failures=$(grep -c 'exited with status\|killed by signal' "$D/$t.$label.h$H.err") iterations=$(grep -c '^ + PE_start' "$D/$t.$label.h$H.out") | $(grep -h 'teams created for PE 0' "$D/$t.$label.h$H.out" | head -1)" >> "$OUT"
}
for t in $PROGRAMS; do
  [ $t = collect_active_set ] && continue
  one $t one-node 70
  one $t ten-nodes 70 -m $BLOCK10
done
one collect_active_set one-node-32 32
one collect_active_set four-nodes-32 32 -m $FOUR8
for n in 61 62 64; do
  SHMEM_SYMMETRIC_SIZE=4m OSOE_SHM_CAPACITY_CHECK=0 $L -t 120 -n $n -m $(seq -s, 0 $((n-1))) -- ./split_shared > "$D/split_shared.$n.h$H.out" 2> "$D/split_shared.$n.h$H.err"; rc=$?
  echo "split_shared nodes=$n: rc=$rc valid=$(grep -c 'team valid' "$D/split_shared.$n.h$H.out") invalid=$(grep -c 'team INVALID' "$D/split_shared.$n.h$H.out")" >> "$OUT"
done
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
cd /; rm -rf /tmp/acc
