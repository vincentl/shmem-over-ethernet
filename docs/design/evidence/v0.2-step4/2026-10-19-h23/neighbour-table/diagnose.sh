#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23's diagnosis: does tier 3 at 64 logical nodes fail on the kernel's shared neighbour table?
D=/tmp/h23-before; O=/tmp/h23-diag; mkdir -p $O; cd $D || exit 1
cmake --preset tier3 > $O/cfg.txt 2>&1 && cmake --build build/tier3 -j4 > $O/build.txt 2>&1 || { echo BUILD FAILED; exit 1; }
echo "built, warnings $(grep -c warning: $O/build.txt)"
fulls() { awk 'NR==1 { for (i=1;i<=NF;i++) if ($i=="table_fulls") c=i; next } { s += strtonum("0x" $c) } END { print s }' /proc/net/stat/arp_cache; }
run() {   # run NODES LABEL
  local n=$1 label=$2 map; map=$(seq -s, 0 $((n - 1)))
  local f0; f0=$(fulls); local t0; t0=$(date +%s.%N)
  sudo -n tools/launch/tier3.sh pi $n SHMEM_SYMMETRIC_SIZE=8m -- build/tier3/tools/launch/osoe-launch -n $n -m $map -t ${TIMEOUT:-150} -- build/tier3/tools/bench/osoe-bench barrier quick > $O/out-$label.txt 2> $O/err-$label.txt
  local s=$?; local t1; t1=$(date +%s.%N)
  echo "$label nodes=$n status=$s seconds=$(echo "$t1 - $t0" | bc) table_fulls_added=$(( $(fulls) - f0 )) notices=$(grep -c 'has waited' $O/err-$label.txt)"
}
sudo -n dmesg -C 2>/dev/null
echo "gc_thresh3 $(sysctl -n net.ipv4.neigh.default.gc_thresh3)"
run 32 n32-default
run 64 n64-default
sudo -n dmesg | grep -c "table overflow" | sed 's/^/dmesg overflow lines: /'
sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=16384 net.ipv4.neigh.default.gc_thresh3=32768
echo "gc_thresh3 $(sysctl -n net.ipv4.neigh.default.gc_thresh3) (raised for the run)"
run 64 n64-raised
run 128 n128-raised
sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh2=512 net.ipv4.neigh.default.gc_thresh3=1024
echo "gc_thresh3 $(sysctl -n net.ipv4.neigh.default.gc_thresh3) (restored)"
sudo -n tools/launch/tier3.sh clean
echo "DIAG DONE $(date +%H:%M:%S)"
