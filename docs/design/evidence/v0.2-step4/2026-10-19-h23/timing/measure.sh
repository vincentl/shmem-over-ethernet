#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's start-up timing: each lead's "connected its mesh in X us" (SHMEM_DEBUG)
# at each node count, over tier 3's Pi-like link and over loopback.
# measure.sh TREE LABEL ROUNDS LINKS COUNTS... (LINKS: tier3, loopback or both)
D=$1; L=$2; R=$3; LINKS=$4; shift 4; O=/tmp/h23-timing; mkdir -p $O
cd "$D" || exit 1
B=$D/build/tier3
restore() { sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=512 net.ipv4.neigh.default.gc_thresh3=1024; }
trap restore EXIT
sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=32768 net.ipv4.neigh.default.gc_thresh3=65536
for n in "$@"; do
  map=$(seq -s, 0 $((n - 1)))
  for r in $(seq 1 "$R"); do
    for link in $LINKS; do
      f=$O/$L-$link-$n-$r
      t0=$(date +%s%N)
      if [ "$link" = tier3 ]; then
        sudo -n tools/launch/tier3.sh pi "$n" SHMEM_DEBUG=1 SHMEM_SYMMETRIC_SIZE=8m -- \
          "$B/tools/launch/osoe-launch" -n "$n" -m "$map" -t 300 -- "$B/tools/bench/osoe-bench" barrier quick > "$f.out" 2> "$f.err"
      else
        env SHMEM_DEBUG=1 SHMEM_SYMMETRIC_SIZE=8m "$B/tools/launch/osoe-launch" -n "$n" -m "$map" -t 300 -- \
          "$B/tools/bench/osoe-bench" barrier quick > "$f.out" 2> "$f.err"
      fi
      s=$?; t1=$(date +%s%N)
      most=$(grep -o 'connected its mesh in [0-9]* us' "$f.err" | awk '{ print $5 }' | sort -n | tail -1)
      median=$(grep -o 'connected its mesh in [0-9]* us' "$f.err" | awk '{ print $5 }' | sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }')
      echo "$L link=$link nodes=$n round=$r status=$s job_ms=$(( (t1 - t0) / 1000000 )) mesh_us_most=$most mesh_us_median=$median leads=$(grep -c 'connected its mesh' "$f.err")"
    done
  done
done
