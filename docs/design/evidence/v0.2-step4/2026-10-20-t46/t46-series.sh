#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T46: the one-second tail at 128 logical nodes on tier 3.
#
#   sudo -n flock -w 3600 /tmp/osoe-vm.lock t46-series.sh TREE OUTPUT LABEL SHAPE NODES RUNS [OPTION]...
#
# RUNS jobs of `osoe-bench barrier quick` over NODES logical nodes on tier 3's
# link SHAPE (pi, lossy), with SHMEM_DEBUG=1 - whose "connected its mesh in"
# lines name the late leads - and, around each job, every namespace's counters
# and the host's (t46-wrapper.sh, t46-snapshot.py). Each run is a directory
# OUTPUT/LABEL/run-NN; the line t46-report.py --brief prints for it is printed
# here as it ends. TREE is a tree built with the tier3 preset
# (cmake --preset tier3 && cmake --build build/tier3).
#
# Options:
#   capture=1     capture the ARP and TCP handshake packets of every node
#                 (t46-sniff.py) beside each job
#   backlog=N     net.core.netdev_max_backlog set to N for the series
#   null=SECONDS  no job: sleep SECONDS in the namespaces instead, for the
#                 counters' own drift
#   portloss=NODE:LOSS[,NODE:LOSS]...   netem loss on the bridge's port to each
#                 NODE (say 5:2%): frames lost after the sender's capture and
#                 before the receiver's, as a full backlog queue would lose them
#   static=1      every namespace is given a permanent neighbour entry for every
#                 other, so that no ARP is sent: a control for ARP's part
#   start=N       number the runs from N, so that a label's runs can be made by
#                 several calls (alternating with another label's)
#
# tier3.sh refuses more nodes than the kernel's shared neighbour table holds,
# so this raises net.ipv4.neigh.default.gc_thresh2 and gc_thresh3 for the
# series (to 32768 and 65536, as H23's measure.sh did), and puts back whatever
# it found - and net.core.netdev_max_backlog - when it ends, however it ends
# but by SIGKILL. Run by hand, if it was killed so:
#   sysctl -w net.ipv4.neigh.default.gc_thresh2=512 net.ipv4.neigh.default.gc_thresh3=1024 \
#             net.core.netdev_max_backlog=1000
set -u
if [ $# -lt 6 ]; then
    echo "usage: t46-series.sh TREE OUTPUT LABEL SHAPE NODES RUNS [capture=1] [backlog=N] [null=SECONDS] [portloss=NODE:LOSS,...] [static=1] [start=N]" >&2
    exit 2
fi
TREE=$1 OUTPUT=$2 LABEL=$3 SHAPE=$4 NODES=$5 RUNS=$6
shift 6
HERE=$(cd "$(dirname "$0")" && pwd)
BACKLOG= CAPTURE= NULL= PORT_LOSS= STATIC= START=1
for option in "$@"; do
    case $option in
        backlog=*) BACKLOG=${option#backlog=} ;;
        capture=1) CAPTURE=1 ;;
        null=*)    NULL=${option#null=} ;;
        portloss=*) PORT_LOSS=${option#portloss=} ;;
        static=1)  STATIC=1 ;;
        start=*)   START=${option#start=} ;;
        *)         echo "t46-series.sh: unknown option $option" >&2; exit 2 ;;
    esac
done
if [ "$(id -u)" != 0 ] || [ -z "${SUDO_USER:-}" ]; then
    echo "t46-series.sh: run it with sudo -n: osoe-launch returns each rank to SUDO_USER" >&2
    exit 2
fi

found_gc_thresh1=$(sysctl -n net.ipv4.neigh.default.gc_thresh1)
found_gc_thresh2=$(sysctl -n net.ipv4.neigh.default.gc_thresh2)
found_gc_thresh3=$(sysctl -n net.ipv4.neigh.default.gc_thresh3)
found_backlog=$(sysctl -n net.core.netdev_max_backlog)
restore() {
    sysctl -q -w net.ipv4.neigh.default.gc_thresh1="$found_gc_thresh1" \
        net.ipv4.neigh.default.gc_thresh2="$found_gc_thresh2" \
        net.ipv4.neigh.default.gc_thresh3="$found_gc_thresh3" \
        net.core.netdev_max_backlog="$found_backlog"
    echo "restored: gc_thresh1=$(sysctl -n net.ipv4.neigh.default.gc_thresh1)" \
         "gc_thresh2=$(sysctl -n net.ipv4.neigh.default.gc_thresh2)" \
         "gc_thresh3=$(sysctl -n net.ipv4.neigh.default.gc_thresh3)" \
         "netdev_max_backlog=$(sysctl -n net.core.netdev_max_backlog)"
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

sysctl -q -w net.ipv4.neigh.default.gc_thresh2=32768 net.ipv4.neigh.default.gc_thresh3=65536
[ -z "$BACKLOG" ] || sysctl -q -w net.core.netdev_max_backlog="$BACKLOG"
mkdir -p "$OUTPUT/$LABEL"
{
    echo "found: gc_thresh1=$found_gc_thresh1 gc_thresh2=$found_gc_thresh2 gc_thresh3=$found_gc_thresh3 netdev_max_backlog=$found_backlog"
    echo "set:   gc_thresh1=$(sysctl -n net.ipv4.neigh.default.gc_thresh1) gc_thresh2=$(sysctl -n net.ipv4.neigh.default.gc_thresh2) gc_thresh3=$(sysctl -n net.ipv4.neigh.default.gc_thresh3) netdev_max_backlog=$(sysctl -n net.core.netdev_max_backlog)"
    echo "series: label=$LABEL shape=$SHAPE nodes=$NODES runs=$RUNS from=$START capture=${CAPTURE:-0} backlog=${BACKLOG:-default} null=${NULL:-0} portloss=${PORT_LOSS:-none} static=${STATIC:-0}"
    echo "kernel: $(uname -r)  cpus: $(nproc)  $(date '+%F %T')"
} | tee "$OUTPUT/$LABEL/series.txt"

map=$(seq -s, 0 $((NODES - 1)))
B=$TREE/build/tier3
python3 "$HERE/t46-report.py" --header | tee -a "$OUTPUT/$LABEL/series.txt"
for run in $(seq "$START" $((START + RUNS - 1))); do
    RUNDIR=$OUTPUT/$LABEL/run-$(printf %02d "$run")
    mkdir -p "$RUNDIR"
    job_environment=(SHMEM_DEBUG=1 SHMEM_SYMMETRIC_SIZE=8m T46_RUNDIR="$RUNDIR" T46_NODES="$NODES"
                     T46_DIR="$HERE")
    [ -z "$CAPTURE" ] || job_environment+=(T46_CAPTURE=1)
    [ -z "$NULL" ] || job_environment+=(T46_NULL="$NULL")
    [ -z "$PORT_LOSS" ] || job_environment+=(T46_PORT_LOSS="$PORT_LOSS")
    [ -z "$STATIC" ] || job_environment+=(T46_STATIC=1)
    "$TREE/tools/launch/tier3.sh" "$SHAPE" "$NODES" "${job_environment[@]}" -- \
        "$HERE/t46-wrapper.sh" "$B/tools/launch/osoe-launch" -n "$NODES" -m "$map" -t 300 -- \
        "$B/tools/bench/osoe-bench" barrier quick > "$RUNDIR/out.txt" 2> "$RUNDIR/err.txt"
    echo $? > "$RUNDIR/tier3-status.txt"
    python3 "$HERE/t46-report.py" --brief "$RUNDIR" | tee -a "$OUTPUT/$LABEL/series.txt"
done
left=$(ip netns list | grep -c '^osoe-tier3-')
echo "series done $(date '+%T'); namespaces left behind: $left" | tee -a "$OUTPUT/$LABEL/series.txt"
