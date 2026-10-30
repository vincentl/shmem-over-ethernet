#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P4 on the Pi cluster (docs/prompts/v0.2-open-items.md): on two nodes, the tree
# against Träff's for a byte sum at 16 KiB k and 16 KiB k + 1 (two-node.c), one
# PE a node and four, in three arms run one after another, -K1 --cpu-bind=none:
#   tree   OSOE_COLLECTIVE_SHAPE=tree, on the link below;
#   traff  the shape `link` on a link set so that Träff's is the cheaper at every
#          size measured - a hop of 10 us, no message cost, the Pi's 116 MB/s,
#          which leaves every data wire's unsent limit at its 16 KiB floor, as
#          the Pi's measured link does - the tree's arm on the same link, so the
#          two differ in the algorithm alone;
#   built  nothing set: the link measured as the job starts, and the chooser as
#          built, rank 0 printing the link (OSOE_TEST_PRINT_LINK=1).
# Three series on pi-001 and pi-002, a fourth on pi-003 and pi-004; every
# node's uptime and offload (tso, gso) before and after.
#   two-node.sh OUT   - on the head node, from ~/osoe, after the release build
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
run() {   # run NAME HOSTS PES-A-NODE ENVIRONMENT
    timeout 600 srun -K1 --cpu-bind=none -N2 -w "$2" --ntasks-per-node="$3" \
        env SHMEM_SYMMETRIC_SIZE=32m $4 "$OUT/two-node" > "$OUT/$1.txt" 2>&1 < /dev/null
    say "$1 exit $?: $(tail -1 "$OUT/$1.txt")"
}
for series in 1 2 3 4; do
    hosts=pi-001,pi-002
    [ $series = 4 ] && hosts=pi-003,pi-004
    for pes in 1 4; do
        run tree-2x$pes-$series $hosts $pes "OSOE_COLLECTIVE_SHAPE=tree $LINK"
        run traff-2x$pes-$series $hosts $pes "OSOE_COLLECTIVE_SHAPE=link $LINK"
        run built-2x$pes-$series $hosts $pes "OSOE_TEST_PRINT_LINK=1"
    done
done
state
say DONE
