#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# teams70.sh: T4 of the 0.2 tag's checks - the release checklist's acceptance past 61 PEs at
# OSOE_PROGRESS_HELPERS=H, as v0.2 step 4's merge point ran it (its acceptance.sh), on the tag tree's
# release build (build.sh) at the default OSOE_MAX_TEAMS: the SOS team and active-set programs at 70 PEs
# on one node and on ten logical nodes of seven; collect_active_set at 32 on one node and on four of
# eight (its own limit); and the per-node split of SHMEM_TEAM_SHARED (split_shared) at 61, 62 and 64
# nodes of one PE as there, and at 70 PEs: 70 nodes of one, ten nodes of seven, and one node.
# The caller holds the VM's lock.
#   teams70.sh H
set -u
H=${1:?helpers}
HERE=$(cd "$(dirname "$0")" && pwd)
R=<repo>
C=${T4_SOS:-$R/corpus/tests-sos/test}/unit
S=${T4_SPLIT:-$R/build/scratch/review-r3/scaling/split_shared.c}
TREE=${T4_TREE:-/var/tmp/t4-tree}
W=${T4_WORK:-/var/tmp/t4-teams-h$H}
OUTDIR=${T4_OUT:-$HERE}
D=$OUTDIR/teams70-h$H
O=$OUTDIR/teams70-h$H.txt
O1=$TREE/build/release/tools/wrapper/oshcc
L=$TREE/build/release/tools/launch/osoe-launch
export OSOE_PROGRESS_HELPERS=$H
rm -rf "$W" "$D" && mkdir -p "$W" "$D"
: > "$O"
echo "== T4 team programs at 70 PEs, OSOE_PROGRESS_HELPERS=$H, tree $(cat "$TREE/.t4-commit"), $(date -u +%FT%TZ), load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$O"
env | grep -E '^(OSOE|SHMEM|SMA)_' >> "$O"
ls /dev/shm | sort > "$W/shm-before.txt"
cd "$W"
PROGRAMS="shmem_team_reuse_teams reduce_active_set broadcast_active_set collect_active_set repeated_barriers shmem_team_max shmem_team_split_2d"
for t in $PROGRAMS; do
  $O1 -O1 -w -o $t $C/$t.c 2> $D/$t.build || { echo "$t: BUILD FAILED" >> "$O"; continue; }
done
$O1 -O1 -o split_shared $S 2> $D/split_shared.build || echo "split_shared: BUILD FAILED" >> "$O"
BLOCK10=$(for n in $(seq 0 9); do for k in 1 2 3 4 5 6 7; do printf "%d," $n; done; done | sed 's/,$//')
FOUR8=$(for n in 0 1 2 3; do for k in 1 2 3 4 5 6 7 8; do printf "%d," $n; done; done | sed 's/,$//')
ONES() { seq -s, 0 $(( $1 - 1 )); }
one() {   # program label PEs map-arguments...
  local t=$1 label=$2 n=$3 rc t0 t1; shift 3
  [ -x ./$t ] || return
  t0=$(date +%s.%N)
  SHMEM_SYMMETRIC_SIZE=4m OSOE_SHM_CAPACITY_CHECK=0 timeout 400 $L -t 300 -n $n "$@" -- ./$t > "$D/$t.$label.out" 2> "$D/$t.$label.err" < /dev/null; rc=$?
  t1=$(date +%s.%N)
  echo "$t $label: rc=$rc ${n}PEs $(echo "$t0 $t1" | awk '{printf "%.1fs", $2 - $1}') no-number=$(grep -c 'no team number is free\|no two team numbers are free' "$D/$t.$label.err") dry=$(grep -c 'has no team block left' "$D/$t.$label.err") rank-failures=$(grep -c 'exited with status\|killed by signal' "$D/$t.$label.err") iterations=$(grep -c '^ + PE_start' "$D/$t.$label.out") out-lines=$(wc -l < "$D/$t.$label.out") err-lines=$(wc -l < "$D/$t.$label.err") failure-words=$(cat "$D/$t.$label.out" "$D/$t.$label.err" | grep -c -i 'fail\|error\|mismatch\|invalid\|abort\|expected') | $(grep -h 'teams created for PE 0' "$D/$t.$label.out" | head -1)" >> "$O"
}
for t in $PROGRAMS; do
  [ $t = collect_active_set ] && continue
  one $t one-node 70
  one $t ten-nodes 70 -m $BLOCK10
done
one collect_active_set one-node-32 32
one collect_active_set four-nodes-32 32 -m $FOUR8
split() {   # label PEs map
  local label=$1 n=$2 map=$3 rc t0 t1
  t0=$(date +%s.%N)
  SHMEM_SYMMETRIC_SIZE=4m OSOE_SHM_CAPACITY_CHECK=0 timeout 200 $L -t 120 -n $n ${map:+-m $map} -- ./split_shared > "$D/split_shared.$label.out" 2> "$D/split_shared.$label.err" < /dev/null; rc=$?
  t1=$(date +%s.%N)
  echo "split_shared $label: rc=$rc ${n}PEs $(echo "$t0 $t1" | awk '{printf "%.1fs", $2 - $1}') valid=$(grep -c 'team valid' "$D/split_shared.$label.out") invalid=$(grep -c 'team INVALID' "$D/split_shared.$label.out") nonzero-returns=$(grep -c 'returned [1-9-]' "$D/split_shared.$label.out") out-lines=$(wc -l < "$D/split_shared.$label.out") err-lines=$(wc -l < "$D/split_shared.$label.err")" >> "$O"
}
for n in 61 62 64; do split nodes=$n $n "$(ONES $n)"; done
split nodes=70 70 "$(ONES 70)"
split ten-nodes-of-7=70 70 "$BLOCK10"
split one-node=70 70 ""
ls /dev/shm | sort > "$W/shm-after.txt"
{
  echo "== summary (helpers $H)"
  echo "runs: $(grep -c ': rc=' "$O"), rc=0: $(grep -c ': rc=0 ' "$O"), not rc=0: $(grep ': rc=' "$O" | grep -vc ': rc=0 ')"
  echo "/dev/shm entries new since the start: $(comm -13 "$W/shm-before.txt" "$W/shm-after.txt" | wc -l)"
} >> "$O"
echo DONE >> "$O"
