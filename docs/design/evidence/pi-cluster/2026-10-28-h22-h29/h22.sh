#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H22 on the Pi cluster: F1's control (docs/pi-cluster-results.md, "F1, investigated" - with
# pi-002's segmentation offload off and every node just booted, no stalls; offload back on,
# on nodes as freshly booted, tells the offload from the boot). F1's trigger map: f1probe -
# shmem_fcollectmem over the world, 200 calls after 20, a call over 20 ms counted slow - at
# 4 x 1, 4 x 2 and 4 x 4 over F1's block sizes, twice, and with rank 0 on pi-002; each node's
# TCP retransmission timeouts before and after each run. LABEL names the run (off, on).
#   h22.sh LABEL        - on the head node, from ~/osoe
set -u
LABEL=${1:?LABEL}
B=~/osoe/build/h22-h29; O=$B/h22-$LABEL; mkdir -p "$O"
say() { echo "$(date +%T) $*" | tee -a "$O/summary.txt"; }
nodes() { srun -N4 --ntasks-per-node=1 sh -c "$1" 2>/dev/null | sort | tr '\n' ' '; }
timeouts() { nodes 'echo "$(hostname)=$(nstat -az TcpExtTCPTimeouts | awk "NR>1{print \$2}")"'; }
: > "$O/summary.txt"
say "tree $(cat ~/osoe/.osoe-commit); label $LABEL"
say "uptime: $(nodes 'echo "$(hostname)=$(cut -d. -f1 /proc/uptime)s"')"
say "offload: $(nodes 'echo "$(hostname)=$(/usr/sbin/ethtool -k eth0 | awk "/^tcp-segmentation-offload:|^generic-segmentation-offload:/ {printf \"%s\", \$2 == \"on\" ? \"1\" : \"0\"}")"') (tso,gso; 1 on)"
run() {   # run NAME SRUN-ARGS... -- BLOCK...
    local name=$1 args=() before
    shift
    while [ "$1" != "--" ]; do args+=("$1"); shift; done
    shift
    before=$(timeouts)
    timeout 900 srun -K1 "${args[@]}" sh -c 'echo "rank $SLURM_PROCID on $(hostname)" >&2; exec '"$B"'/f1probe "$@"' f1probe "$@" > "$O/$name.txt" 2>&1 < /dev/null
    say "$name exit $?: $(awk '/^block/ {printf "%s:slow=%s,max_ms=%s ", $2, $8, $10}' "$O/$name.txt")"
    say "    timeouts before: $before"
    say "    timeouts after:  $(timeouts)"
}
for r in a b; do
    run A-4x1-$r -N4 --ntasks-per-node=1 -- 16384 20480 24576 28672 32768 36864 40960 49152
    run A-4x2-$r -N4 --ntasks-per-node=2 -- 8192 12288 16384 20480 24576
    run A-4x4-$r -N4 --ntasks-per-node=4 -- 4096 8192 12288 16384
done
printf '%s\n' pi-002 pi-001 pi-003 pi-004 > "$O/hosts"
SLURM_HOSTFILE=$O/hosts run P-rank0-on-pi-002 -n4 --distribution=arbitrary -- 28672 32768 28672 32768
say DONE
