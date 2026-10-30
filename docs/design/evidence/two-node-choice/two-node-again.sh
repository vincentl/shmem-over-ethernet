#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P4's second run on the Pi cluster, after two-node.sh: its series agreed within
# a few per cent at 256 KiB and up but spread by up to a fifth from 48 to
# 128 KiB, and its arm as built - the link measured - ran the tree faster than
# the tree's arm on the link set. So: six series on pi-001 and pi-002, one PE a
# node and four, the arms' order turned one place each series, and a third arm
#   tree-measured  OSOE_COLLECTIVE_SHAPE=tree, the link measured as the job
#                  starts (OSOE_TEST_PRINT_LINK=1), against `tree` on the link
#                  set: whether setting the link changes the tree's own time;
# beside two-node.sh's tree and traff, unchanged. Every node's uptime and
# offload (tso, gso) before and after.
#   two-node-again.sh OUT   - on the head node, from ~/osoe, after two-node.sh
set -u
OUT=${1:?OUT}
mkdir -p "$OUT"
say() { echo "$(date +%T) $*" | tee -a "$OUT/summary.txt"; }
nodes() { srun -N4 --ntasks-per-node=1 sh -c "$1" 2>/dev/null | sort | tr '\n' ' '; }
state() {
    say "uptime: $(nodes 'echo "$(hostname)=$(cut -d. -f1 /proc/uptime)s"')"
    say "offload: $(nodes 'echo "$(hostname)=$(/usr/sbin/ethtool -k eth0 | awk "/^tcp-segmentation-offload:|^generic-segmentation-offload:/ {printf \"%s\", \$2 == \"on\" ? \"1\" : \"0\"}")"') (tso,gso; 1 on)"
}
: > "$OUT/summary.txt"
say "tree $(cat .osoe-commit)"
build/release/tools/wrapper/oshcc -O2 -I shmem/tests -I tools/stat -I shmem/src \
    -o "$OUT/two-node" build/p4/two-node.c tools/stat/segment.c > "$OUT/probe-build.txt" 2>&1
say "probe built: $?, warnings: $(grep -c 'warning:' "$OUT/probe-build.txt")"
state
LINK="OSOE_LINK_HOP_NS=10000 OSOE_LINK_MESSAGE_NS=0 OSOE_LINK_BYTES_PER_SECOND=116000000"
run() {   # run NAME PES-A-NODE ENVIRONMENT
    timeout 600 srun -K1 --cpu-bind=none -N2 -w pi-001,pi-002 --ntasks-per-node="$2" \
        env SHMEM_SYMMETRIC_SIZE=32m $3 "$OUT/two-node" > "$OUT/$1.txt" 2>&1 < /dev/null
    say "$1 exit $?: $(tail -1 "$OUT/$1.txt")"
}
arms=(tree traff tree-measured)
for series in 1 2 3 4 5 6; do
    for pes in 1 4; do
        for i in 0 1 2; do
            arm=${arms[$(( (i + series) % 3 ))]}
            case $arm in
            tree) environment="OSOE_COLLECTIVE_SHAPE=tree $LINK" ;;
            traff) environment="OSOE_COLLECTIVE_SHAPE=link $LINK" ;;
            tree-measured) environment="OSOE_COLLECTIVE_SHAPE=tree OSOE_TEST_PRINT_LINK=1" ;;
            esac
            run $arm-2x$pes-$series $pes "$environment"
        done
    done
done
state
say DONE
