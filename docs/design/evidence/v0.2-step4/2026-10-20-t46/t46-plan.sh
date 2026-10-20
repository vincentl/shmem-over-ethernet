#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T46: the whole plan, in one piece, so that the VM's lock is taken once.
#
#   flock -w 43200 /tmp/osoe-vm.lock bash t46-plan.sh TREE OUTPUT [NODES [dry]]
#
# TREE is the repository's tree extracted with git archive (the VM has no git);
# OUTPUT is where the records go; NODES (128) is the node count, and `dry` makes
# every series one run, to try the plan out on another machine. Run as the
# ordinary user: the build is its
# own, and each series is run by `sudo -n t46-series.sh` (which raises and puts
# back the kernel's neighbour-table limits, and the backlog queue's where a
# series sets it). A step that fails is noted in OUTPUT/progress.txt and the
# next goes on; the build's failure ends the plan.
#
# The steps, at NODES (128) logical nodes over tier 3's Pi-like link unless said:
#   smoke    16 nodes, captured: the harness against the real binaries
#   null     no job, 4 s of sleep: what the counters do by themselves
#   base     10 runs of `osoe-bench barrier quick`
#   capture  8 runs, the ARP and handshake packets of every node captured
#   lossy    3 runs on tier 3's `lossy` link (0.1% of packets): a control for the counters
#   portloss 2 runs captured, with 2% of the frames to nodes 5 and 70 (11/20 of the
#            way along) lost on the bridge's ports: a control for the capture, which
#            sees loss after the sender's tap
#   ab       6 pairs of runs, net.core.netdev_max_backlog at its default and at
#            65536, alternating
#   static   6 runs with permanent neighbour entries, so that no ARP is sent
set -u
TREE=$1
OUTPUT=$2
NODES=${3:-128}
DRY=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUTPUT"
echo $$ > "$OUTPUT/plan.pid"
progress() { echo "$(date +%T) $*" | tee -a "$OUTPUT/progress.txt"; }

{
    echo "plan started $(date '+%F %T') on $(hostname)"
    uname -a
    lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core|Socket|Hypervisor|Virtualization'
    free -m
    uptime
    vmstat 1 3
    grep -E 'CONFIG_HZ=|CONFIG_NET_SCH_NETEM|CONFIG_VETH|CONFIG_BRIDGE=|CONFIG_RPS|CONFIG_NF_CONNTRACK=' \
        "/boot/config-$(uname -r)" 2>/dev/null
    echo "ip: $(ip -V)  tc: $(tc -V)  python: $(python3 --version)"
    sysctl net.ipv4.neigh.default.gc_thresh1 net.ipv4.neigh.default.gc_thresh2 \
        net.ipv4.neigh.default.gc_thresh3 net.ipv4.neigh.default.gc_interval \
        net.ipv4.neigh.default.retrans_time_ms net.ipv4.neigh.default.mcast_solicit \
        net.ipv4.neigh.default.unres_qlen_bytes net.ipv4.neigh.default.proxy_qlen \
        net.core.netdev_max_backlog net.core.netdev_budget net.core.netdev_budget_usecs \
        net.core.dev_weight net.core.somaxconn net.ipv4.tcp_max_syn_backlog \
        net.ipv4.tcp_syncookies net.ipv4.tcp_syn_retries net.ipv4.tcp_synack_retries \
        net.ipv4.ip_local_port_range net.ipv4.conf.all.arp_ignore net.ipv4.conf.all.arp_accept \
        net.ipv4.conf.default.arp_accept 2>&1
    echo "dmesg tail:"
    sudo -n dmesg | tail -5
} > "$OUTPUT/environment.txt" 2>&1

# Whatever the plan finds in the kernel's settings is put back when it ends, however it ends
# (each series does the same for its own part).
read -r found_gc_thresh1 found_gc_thresh2 found_gc_thresh3 found_backlog < <(
    sysctl -n net.ipv4.neigh.default.gc_thresh1 net.ipv4.neigh.default.gc_thresh2 \
        net.ipv4.neigh.default.gc_thresh3 net.core.netdev_max_backlog | tr '\n' ' ')
restore() {
    sudo -n sysctl -q -w net.ipv4.neigh.default.gc_thresh1="$found_gc_thresh1" \
        net.ipv4.neigh.default.gc_thresh2="$found_gc_thresh2" \
        net.ipv4.neigh.default.gc_thresh3="$found_gc_thresh3" net.core.netdev_max_backlog="$found_backlog"
    progress "settings put back: gc_thresh1 $(sysctl -n net.ipv4.neigh.default.gc_thresh1)," \
        "gc_thresh2 $(sysctl -n net.ipv4.neigh.default.gc_thresh2)," \
        "gc_thresh3 $(sysctl -n net.ipv4.neigh.default.gc_thresh3)," \
        "netdev_max_backlog $(sysctl -n net.core.netdev_max_backlog)" \
        "(found $found_gc_thresh1, $found_gc_thresh2, $found_gc_thresh3, $found_backlog)"
    date '+%F %T' > "$OUTPUT/DONE"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

progress "building the tier3 preset"
cd "$TREE" || exit 1
cmake --preset tier3 > "$OUTPUT/configure.txt" 2>&1 &&
    cmake --build build/tier3 -j4 --target osoe-launch osoe-bench > "$OUTPUT/build.txt" 2>&1 || {
    progress "THE BUILD FAILED"
    exit 1
}
progress "built, $(grep -c 'warning:' "$OUTPUT/build.txt") warnings"

series() {      # series LABEL SHAPE NODES RUNS [OPTION]...
    progress "series $*"
    sudo -n bash "$HERE/t46-series.sh" "$TREE" "$OUTPUT" "$@" > "$OUTPUT/series-$1-$(date +%H%M%S).log" 2>&1
    progress "series $1 ended with status $?"
}

runs() { if [ -n "$DRY" ]; then echo 1; else echo "$1"; fi; }
series smoke pi 16 1 capture=1
series null pi "$NODES" "$(runs 2)" null=4
series base pi "$NODES" "$(runs 10)"
series capture pi "$NODES" "$(runs 8)" capture=1
series lossy lossy "$NODES" "$(runs 3)"
series portloss pi "$NODES" "$(runs 2)" capture=1 portloss=5:2%,$((NODES * 11 / 20)):2%
for pair in $(seq 1 "$(runs 6)"); do
    series ab-default pi "$NODES" 1 start=$pair
    series ab-backlog pi "$NODES" 1 backlog=65536 start=$pair
done
series static pi "$NODES" "$(runs 6)" static=1

left=$(ip netns list | grep -c '^osoe-tier3-')
progress "plan done; namespaces left behind: $left"
