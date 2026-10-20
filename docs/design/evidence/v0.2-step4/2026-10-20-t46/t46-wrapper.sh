#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# T46's launcher shim. tier3.sh runs its LAUNCHER as
#
#     LAUNCHER -N PREFIX -H HOSTS ARGS...
#
# and t46-series.sh names this script as that launcher, with the real one -
# osoe-launch - as the first of ARGS. The shim reads the counters of every
# namespace and of the host (t46-snapshot.py), starts the capture if asked
# (t46-sniff.py), runs the real launcher, reads the counters again, and returns
# the launcher's status. Everything it keeps is in T46_RUNDIR:
#
#   before.json.gz, after.json.gz   the counters, either side of the job
#   capture.txt.gz                  the capture's events (T46_CAPTURE)
#   nstat-node0.txt                 `nstat -asz` in node 0's namespace after the job, to check
#                                   that the snapshots read what nstat reads
#   dmesg.txt                       the kernel log lines written during the job
#   job.txt                         the job's status, and when it began and ended
#   wrapper.log                     what the shim's helpers said
#
# The environment (t46-series.sh gives it to tier3.sh as NAME=VALUE arguments):
#   T46_RUNDIR   the directory above
#   T46_NODES    the number of logical nodes
#   T46_DIR      the directory of these scripts
#   T46_CAPTURE  set: capture the ARP and handshake packets beside the job
#   T46_NULL     set to N seconds: no job, a sleep of N seconds - the counters'
#                own drift in namespaces that nothing uses
#   T46_STATIC   set: before the counters are read, every namespace is given a
#                permanent neighbour entry for each other, so that no ARP is sent
#   T46_PORT_LOSS  NODE:LOSS[,NODE:LOSS]...  netem loss on the bridge's port to each
#                NODE, say 5:2%, so that frames to that node are lost after the sender's
#                capture and before the receiver's - a control for the capture and for
#                the counters; the ports' netem counts are left in port-netem.txt
set -u
if [ $# -lt 4 ] || [ "$1" != -N ] || [ "$3" != -H ]; then
    echo "t46-wrapper.sh: expected -N PREFIX -H HOSTS LAUNCHER ARGS..." >&2
    exit 2
fi
PREFIX=$2
HOSTS=$4
shift 4
RUNDIR=${T46_RUNDIR:?}
LOG=$RUNDIR/wrapper.log
sniffer=
stop_sniffer() {
    if [ -n "$sniffer" ]; then
        kill -TERM "$sniffer" 2>/dev/null
        wait "$sniffer" 2>/dev/null
        sniffer=
    fi
}
trap stop_sniffer EXIT

for port_loss in $(echo "${T46_PORT_LOSS:-}" | tr , ' '); do
    ip netns exec "${PREFIX}switch" tc qdisc add dev "port${port_loss%%:*}" root netem loss "${port_loss#*:}"
done

if [ -n "${T46_STATIC:-}" ]; then
    # Each node's link0 address, then for each node one batch of 'neigh replace' lines.
    addresses=$RUNDIR/link0-addresses.txt
    node=0
    : > "$addresses"
    while [ "$node" -lt "$T46_NODES" ]; do
        echo "$(ip -n "$PREFIX$node" -o link show link0 | sed 's/.*link\/ether \([0-9a-f:]*\) .*/\1/')" >> "$addresses"
        node=$((node + 1))
    done
    node=0
    while [ "$node" -lt "$T46_NODES" ]; do
        awk -v me="$node" '{ if (NR - 1 != me) printf "neigh replace 10.99.0.%d lladdr %s dev link0 nud permanent\n", NR, $1 }' \
            "$addresses" | ip -n "$PREFIX$node" -batch - || echo "static neighbours of node $node failed" >> "$LOG"
        node=$((node + 1))
    done
fi

kernel_log_lines=$(dmesg | wc -l)
python3 "$T46_DIR/t46-snapshot.py" "$PREFIX" "$T46_NODES" "$RUNDIR/before.json.gz" >> "$LOG" 2>&1 ||
    echo "the snapshot before failed" >> "$LOG"

if [ -n "${T46_CAPTURE:-}" ]; then
    rm -f "$RUNDIR/capture.ready"
    python3 "$T46_DIR/t46-sniff.py" "$PREFIX" "$T46_NODES" "$RUNDIR/capture.txt" \
        "$RUNDIR/capture.ready" >> "$LOG" 2>&1 &
    sniffer=$!
    waited=0
    while [ ! -e "$RUNDIR/capture.ready" ] && [ "$waited" -lt 300 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    [ -e "$RUNDIR/capture.ready" ] || echo "the capture was not ready after 30 s" >> "$LOG"
fi

started=$(date +%s%N)
if [ -n "${T46_NULL:-}" ]; then
    sleep "$T46_NULL"
    status=0
else
    launcher=$1
    shift
    "$launcher" -N "$PREFIX" -H "$HOSTS" "$@"
    status=$?
fi
finished=$(date +%s%N)

python3 "$T46_DIR/t46-snapshot.py" "$PREFIX" "$T46_NODES" "$RUNDIR/after.json.gz" >> "$LOG" 2>&1 ||
    echo "the snapshot after failed" >> "$LOG"
ip netns exec "${PREFIX}0" nstat -asz > "$RUNDIR/nstat-node0.txt" 2>> "$LOG"
stop_sniffer
[ ! -e "$RUNDIR/capture.txt" ] || gzip -f "$RUNDIR/capture.txt"
rm -f "$RUNDIR/capture.ready"
dmesg | tail -n +$((kernel_log_lines + 1)) > "$RUNDIR/dmesg.txt"
for port_loss in $(echo "${T46_PORT_LOSS:-}" | tr , ' '); do
    echo "port${port_loss%%:*}, loss ${port_loss#*:}:" >> "$RUNDIR/port-netem.txt"
    ip netns exec "${PREFIX}switch" tc -s qdisc show dev "port${port_loss%%:*}" >> "$RUNDIR/port-netem.txt"
done
echo "status=$status started_ns=$started finished_ns=$finished" > "$RUNDIR/job.txt"
exit "$status"
