#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's on-demand data wires on tier 3 at 64 logical nodes (2026-10-27), each
# node in a network namespace of its own over the Pi-like link (tier3.sh pi),
# the kernel's neighbour table raised for the run and set back after, as the
# record of 2026-10-19 has a deliberate larger rehearsal do (64 nodes need
# 64 x 63 entries, past gc_thresh3's 1,024). Each job: osoe-bench's barrier,
# eagerly and on demand, then test-data-wires' ring on demand, which makes
# the collective tree's wires and the ring's.
#   tier3-64.sh TREE ROUNDS
set -u
D=$1 R=$2
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || exit 1
B=$D/build/tier3
O=/tmp/h23-tier3
mkdir -p "$O"
cd "$D" || exit 2
restore() { sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=512 net.ipv4.neigh.default.gc_thresh3=1024; }
trap restore EXIT
sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=32768 net.ipv4.neigh.default.gc_thresh3=65536
n=64
map=$(seq -s, 0 $((n - 1)))
run() {   # run LABEL MODE PROGRAM...
    local label=$1 mode=$2
    shift 2
    local f=$O/$label t0 t1 s
    t0=$(date +%s%N)
    sudo -n tools/launch/tier3.sh pi "$n" SHMEM_DEBUG=1 SHMEM_SYMMETRIC_SIZE=8m \
        OSOE_MESH_DATA_WIRES="$mode" -- "$B/tools/launch/osoe-launch" -n "$n" -m "$map" -t 300 -- \
        "$@" > "$f.out" 2> "$f.err"
    s=$?
    t1=$(date +%s%N)
    local most median wires dialled
    most=$(grep -o 'connected its mesh in [0-9]* us' "$f.err" | awk '{ print $5 }' | sort -n | tail -1)
    median=$(grep -o 'connected its mesh in [0-9]* us' "$f.err" | awk '{ print $5 }' | sort -n |
             awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }')
    wires=$(grep -c 'made its data wire toward node' "$f.err")
    dialled=$(grep -c 'on demand: it dialled' "$f.err")
    echo "$label nodes=$n mode=$mode status=$s job_ms=$(( (t1 - t0) / 1000000 )) mesh_us_most=$most mesh_us_median=$median leads=$(grep -c 'connected its mesh' "$f.err") data_wire_ends_made=$wires dialled=$dialled ok=$(grep -c 'DATA WIRES TEST OK' "$f.out")" | tee -a "$O/summary.txt"
}
: > "$O/summary.txt"
overflows() { sudo -n dmesg 2>/dev/null | grep -c 'neighbor table overflow'; }
before=$(overflows)
for r in $(seq 1 "$R"); do
    run "barrier-eager-$r" eager "$B/tools/bench/osoe-bench" barrier quick
    run "barrier-on-demand-$r" on-demand "$B/tools/bench/osoe-bench" barrier quick
    run "ring-on-demand-$r" on-demand "$B/shmem/tests/shmem_test_data_wires" ring
done
echo "neighbour table overflows during the runs: $(( $(overflows) - before ))" | tee -a "$O/summary.txt"
echo done > /tmp/h23-tier3.status
