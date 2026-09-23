#!/bin/sh
# netem-sweep.sh ECHO_URING [ROUNDS] [MEBIBYTES]
#
# §8.3's experiment for the floor harness: two network namespaces joined by a
# veth pair, the echo server in one and the client in the other, swept from no
# emulation to a Pi-like 1 GbE link. Loopback is the adversarial case for every
# decision that rests on "wire latency dominates memory by two orders of
# magnitude"; this shows where along the sweep that stops being true.
#
# Needs root (sudo) for the namespaces and qdiscs; §8.5's VM has both. Not a
# ctest: it changes the machine's network state, and it cleans up after itself.
set -eu
ECHO=$1
ROUNDS=${2:-5000}
MEBIBYTES=${3:-256}
A=osoe-echo-a
B=osoe-echo-b
cleanup() { sudo ip netns del $A 2>/dev/null || true; sudo ip netns del $B 2>/dev/null || true; }
trap cleanup EXIT
cleanup
sudo ip netns add $A
sudo ip netns add $B
sudo ip link add osoe-va type veth peer name osoe-vb
sudo ip link set osoe-va netns $A
sudo ip link set osoe-vb netns $B
sudo ip -n $A addr add 10.99.0.1/24 dev osoe-va
sudo ip -n $B addr add 10.99.0.2/24 dev osoe-vb
sudo ip -n $A link set osoe-va up
sudo ip -n $B link set osoe-vb up
sudo ip -n $A link set lo up
sudo ip -n $B link set lo up

# "none", then a 1 Gbit/s link at one-way delays from 0 to Pi-like (§5.3
# recalls about 100 us one way for a Pi on 1 GbE; the sweep brackets it).
for shape in none "rate 1gbit" "delay 25us rate 1gbit" "delay 50us rate 1gbit" \
             "delay 100us rate 1gbit"; do
  for side in "$A osoe-va" "$B osoe-vb"; do
    set -- $side
    sudo ip netns exec $1 tc qdisc del dev $2 root 2>/dev/null || true
    [ "$shape" = none ] || sudo ip netns exec $1 tc qdisc add dev $2 root netem $shape limit 100000
  done
  sudo ip netns exec $A "$ECHO" --server 7777 &
  SERVER=$!
  sleep 0.3
  echo "== link: $shape"
  sudo ip netns exec $B "$ECHO" --client 10.99.0.1 7777 --rounds "$ROUNDS" --mebibytes "$MEBIBYTES" |
    grep -v '^small'
  wait $SERVER
done
