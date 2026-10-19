#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# tier3.sh SHAPE NODES [NAME=VALUE]... -- LAUNCHER [LAUNCHER ARGS]...
# tier3.sh clean
#
# Tier 3 (design §8.2, §8.3): one job of NODES logical nodes with every node in
# a network namespace of its own, the namespaces joined through a bridge by veth
# links that netem shapes, where tier 2 runs every node on this machine's
# loopback. SHAPE is the link each node's egress sees:
#
#   pi      1 Gbit/s and 100 us each way - about a Raspberry Pi on 1 GbE (§5.3)
#   lossy   the same, dropping 0.1% of packets
#   small   a 1,280-byte MTU, 1 Gbit/s and 25 us each way
#
# Segmentation offloads are off on every link, so that netem delays, paces and
# drops packets of the MTU's size, as a wire would, rather than the 64 KiB
# bursts a veth pair otherwise carries whole.
#
# Node i's lead listens on 10.99.0.i+1 in its namespace; the NAME=VALUE
# arguments go into the job's environment, and LAUNCHER - osoe-launch - is run
# with -N and -H naming the namespaces and their addresses, then its own
# arguments. Each run has its own namespaces and its own bridge, named by this
# script's pid, so runs can overlap and their identical addresses never meet.
#
# TIER3_CUT=NODE among the NAME=VALUE arguments is this script's, not the
# job's: once the job has created the file OSOE_TEST_READY names in its
# environment - which this script sets - and two seconds later, node NODE's
# link drops everything in both directions, as a cut cable or a powered-off
# host does. No FIN, no reset: the vanished node that only TCP keepalive can
# find (§3.15). The two seconds let what the job last sent be acknowledged,
# retransmissions on the lossy link included: TCP probes only a connection with
# nothing unacknowledged, and one cut with data in flight waits out the
# kernel's retransmission limit instead, some fifteen minutes.
#
# Needs root: run it with sudo -n (§8.5's VM user has it without a password).
# It changes the network state only inside the namespaces it creates, and
# removes them on exit, on failure and on a signal. The launcher returns each
# rank to the user sudo was run by before it starts the program. A run killed
# outright (SIGKILL) cannot clean up; `tier3.sh clean` removes every namespace
# any run left.
set -eu
if [ "${1:-}" = clean ]; then
    for ns in $(ip netns list | awk '/^osoe-tier3-/ { print $1 }'); do
        ip netns del "$ns"
    done
    rm -f /tmp/osoe-tier3-*-ready     # a killed run's cutter then finds none, and gives up
    exit 0
fi
if [ $# -lt 4 ]; then
    echo "usage: tier3.sh SHAPE NODES [NAME=VALUE]... -- LAUNCHER [ARGS]... | tier3.sh clean" >&2
    exit 2
fi
SHAPE=$1
NODES=$2
shift 2
case $SHAPE in
    pi)    NETEM="delay 100us rate 1gbit";           MTU=1500 ;;
    lossy) NETEM="delay 100us rate 1gbit loss 0.1%"; MTU=1500 ;;
    small) NETEM="delay 25us rate 1gbit";            MTU=1280 ;;
    *)     echo "tier3.sh: no link shape '$SHAPE' (pi, lossy, small)" >&2; exit 2 ;;
esac
case $NODES in
    ''|*[!0-9]*) echo "tier3.sh: NODES '$NODES' is not a number" >&2; exit 2 ;;
esac
if [ "$NODES" -lt 1 ] || [ "$NODES" -gt 254 ]; then
    echo "tier3.sh: $NODES nodes; one /24 names 1 to 254" >&2
    exit 2
fi

# Every namespace's record of its neighbours' Ethernet addresses is one table,
# the kernel's: NODES namespaces on one bridge need NODES x (NODES - 1) of its
# entries, and past net.ipv4.neigh.default.gc_thresh3 - 1,024 by default, so
# past 32 nodes - the kernel drops the packets it cannot address, and a job's
# calls time out until its nodes declare each other vanished (64 nodes, H24 and
# H23, 2026-10-19). Refused here by name instead, the limit unchanged: it is
# the host's, not this run's.
NEIGHBOURS=$((NODES * (NODES - 1)))
LIMIT=$(cat /proc/sys/net/ipv4/neigh/default/gc_thresh3 2>/dev/null || echo 0)
if [ "$LIMIT" -gt 0 ] && [ "$NEIGHBOURS" -gt "$LIMIT" ]; then
    echo "tier3.sh: $NODES nodes need $NEIGHBOURS entries in the kernel's neighbour table," \
         "which every namespace shares, and net.ipv4.neigh.default.gc_thresh3 is $LIMIT:" \
         "raise it to $NEIGHBOURS or more, and gc_thresh2 with it, or run fewer nodes" >&2
    exit 2
fi

PREFIX=osoe-tier3-$$-
SWITCH=${PREFIX}switch
CUTTER=
cleanup() {
    if [ -n "$CUTTER" ]; then
        pkill -P "$CUTTER" 2>/dev/null || true      # its sleep, or its tc
        kill "$CUTTER" 2>/dev/null || true
    fi
    rm -f "/tmp/${PREFIX}ready"
    i=0
    while [ $i -lt "$NODES" ]; do
        ip netns del "$PREFIX$i" 2>/dev/null || true
        i=$((i + 1))
    done
    ip netns del "$SWITCH" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

ip netns add "$SWITCH"
ip -n "$SWITCH" link add bridge type bridge
ip -n "$SWITCH" link set bridge up
i=0
while [ $i -lt "$NODES" ]; do
    node=$PREFIX$i
    ip netns add "$node"
    ip link add link0 netns "$node" type veth peer name port$i netns "$SWITCH"
    ip -n "$SWITCH" link set port$i master bridge
    ip -n "$SWITCH" link set port$i up
    ip netns exec "$SWITCH" ethtool -K port$i tso off gso off gro off
    ip -n "$node" addr add 10.99.0.$((i + 1))/24 dev link0
    ip -n "$node" link set link0 mtu "$MTU"
    ip netns exec "$node" ethtool -K link0 tso off gso off gro off
    ip -n "$node" link set link0 up
    ip -n "$node" link set lo up
    # netem's own limit is 1,000 packets, which a 1 Gbit/s link with 100 us of
    # delay fills; the stream would measure the queue rather than the link.
    ip netns exec "$node" tc qdisc add dev link0 root netem $NETEM limit 100000
    i=$((i + 1))
done

CUT=
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
    case $1 in
        TIER3_CUT=*) CUT=${1#TIER3_CUT=} ;;
        *)           export "$1" ;;
    esac
    shift
done
if [ -n "$CUT" ]; then
    case $CUT in
        ''|*[!0-9]*) echo "tier3.sh: TIER3_CUT '$CUT' is not a node" >&2; exit 2 ;;
    esac
    [ "$CUT" -lt "$NODES" ] || { echo "tier3.sh: TIER3_CUT=$CUT of $NODES nodes" >&2; exit 2; }
    export OSOE_TEST_READY="/tmp/${PREFIX}ready"
    # Egress on the node's side and on the bridge's, so nothing arrives either.
    # Bounded: a job that never gets ready leaves no cutter polling for good.
    ( waited=0
      while [ ! -e "$OSOE_TEST_READY" ]; do
          sleep 0.1
          waited=$((waited + 1))
          [ "$waited" -lt 3000 ] || exit 0
      done
      sleep 2
      ip netns exec "$PREFIX$CUT" tc qdisc change dev link0 root netem loss 100%
      ip netns exec "$SWITCH" tc qdisc add dev "port$CUT" root netem loss 100%
      echo "tier3.sh: node $CUT's link now drops everything" >&2 ) &
    CUTTER=$!
fi
[ $# -gt 1 ] || { echo "tier3.sh: no launcher after --" >&2; exit 2; }
shift
LAUNCHER=$1
shift
if [ "$NODES" -eq 1 ]; then
    HOSTS=10.99.0.1
else
    HOSTS="10.99.0.[1-$NODES]"
fi
"$LAUNCHER" -N "$PREFIX" -H "$HOSTS" "$@"
