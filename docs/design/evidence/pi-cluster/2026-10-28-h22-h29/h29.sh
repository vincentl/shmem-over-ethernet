#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H29 on the Pi cluster: a stream both ways at once between two nodes against one way, which
# tells a link that cannot carry both at the full rate from a cost a byte Traff's model lacks
# (docs/decisions/2026-10-12.md, item 5). On two pairs of nodes, three rounds alternating:
# osoe-bench's bandwidth (PE 0's puts, one way) and pairs (both PEs' 1 MiB puts at once) at
# one PE a node with the node's four cores (-c4); and bare TCP, h29-tcp.py, one way and both
# ways for 10 s. The nodes' offload as they boot. Run on the head node, from ~/osoe.
set -u
B=~/osoe/build/h22-h29; O=$B/h29; mkdir -p "$O"
say() { echo "$(date +%T) $*" | tee -a "$O/summary.txt"; }
BENCH=~/osoe/build/release/tools/bench/osoe-bench
: > "$O/summary.txt"
say "tree $(cat ~/osoe/.osoe-commit); offload: $(srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(/usr/sbin/ethtool -k eth0 | awk "/^tcp-segmentation-offload:|^generic-segmentation-offload:/ {printf \"%s\", \$2 == \"on\" ? \"1\" : \"0\"}")"' 2>/dev/null | sort | tr '\n' ' ') (tso,gso; 1 on)"
for pair in pi-001,pi-002 pi-003,pi-004; do
  for round in 1 2 3; do
    tag=${pair//,/-}-r$round
    timeout 600 srun -K1 -w $pair -N2 --ntasks-per-node=1 -c4 $BENCH bandwidth > "$O/bench-oneway-$tag.txt" 2>&1 < /dev/null
    say "osoe one way $tag: $(awk '/^bandwidth put/ {for (i = 1; i <= NF; i++) if ($i ~ /^(bytes|mib_per_s)=/) printf "%s ", $i}' "$O/bench-oneway-$tag.txt")"
    timeout 600 srun -K1 -w $pair -N2 --ntasks-per-node=1 -c4 $BENCH pairs > "$O/bench-bothways-$tag.txt" 2>&1 < /dev/null
    say "osoe both ways $tag: $(grep -E '^pairs' "$O/bench-bothways-$tag.txt" | tr '\n' ' ')"
    for mode in oneway bothways; do
      timeout 120 srun -K1 -w $pair -N2 --ntasks-per-node=1 -c4 python3 $B/h29-tcp.py $mode 10 > "$O/tcp-$mode-$tag.txt" 2>&1 < /dev/null
      say "tcp $mode $tag: $(grep -E '^tcp' "$O/tcp-$mode-$tag.txt" | awk '{print $3, $5, $6}' | tr '\n' ' ')"
    done
  done
done
say DONE
