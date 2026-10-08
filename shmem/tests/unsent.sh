#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The limit on each connection's unsent bytes (v0.2 step 4; wire-settings.h),
# as each node lead's agent says it under SHMEM_DEBUG once the link is known,
# naming the connections it reached - every one, nodes - 1, or a lead that
# limited nothing would pass.
#
#   unsent.sh LAUNCHER PROGRAM MAP measured|set|floor|fixed|by-hand|none|one-node
#
#   measured  the link measured: every lead names one limit, the same, of at
#             least 16 KiB.
#   set       OSOE_LINK_* give a link of 10^9 bytes a second: 128000.
#   floor     the same at the Pi's 116000000: its 128 µs is under 16 KiB, so 16384.
#   fixed     OSOE_COLLECTIVE_SHAPE=fixed: no link, so the floor, 16384.
#   by-hand   OSOE_WIRE_UNSENT_BYTES=65536: 65536, said to be the setting's.
#   none      OSOE_WIRE_UNSENT_BYTES=0: left to the system, said so.
#   one-node  a job of one node has no connections: no lead names a limit.
#
# PROGRAM is any program that initializes, once, and finishes.
set -u
LAUNCHER=$1 PROGRAM=$2 MAP=$3 MODE=$4
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
nodes=$(printf '%s\n' "$MAP" | tr ',' '\n' | sort -u | wc -l | tr -d ' ')
peers=$((nodes - 1))
LINK="OSOE_LINK_HOP_NS=50000 OSOE_LINK_MESSAGE_NS=7000"
case $MODE in
set)      out=$(env SHMEM_DEBUG=1 $LINK OSOE_LINK_BYTES_PER_SECOND=1000000000 \
              "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
floor)    out=$(env SHMEM_DEBUG=1 $LINK OSOE_LINK_BYTES_PER_SECOND=116000000 \
              "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
fixed)    out=$(env SHMEM_DEBUG=1 OSOE_COLLECTIVE_SHAPE=fixed \
              "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
by-hand)  out=$(env SHMEM_DEBUG=1 OSOE_WIRE_UNSENT_BYTES=65536 \
              "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
none)     out=$(env SHMEM_DEBUG=1 OSOE_WIRE_UNSENT_BYTES=0 \
              "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
*)        out=$(env SHMEM_DEBUG=1 "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
esac
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
limited=$(printf '%s\n' "$out" |
    sed -n "s/^osoe: node [0-9]* limits the unsent bytes of each of its \([0-9]*\) connections to \([0-9]*\).*/\1 \2/p")
left=$(printf '%s\n' "$out" |
    sed -n "s/^osoe: node [0-9]* leaves the unsent bytes of its \([0-9]*\) connections to the system.*/\1/p")
named=$(printf '%s\n' "$limited" | grep -c .)
distinct=$(printf '%s\n' "$limited" | grep . | sort -u | wc -l | tr -d ' ')
reached=$(printf '%s\n' "$limited" "$left" | grep . | awk -v p="$peers" '$1 != p' | wc -l | tr -d ' ')
bytes=$(printf '%s\n' "$limited" | head -1 | awk '{print $2}')
echo "leads naming a limit: $named of $nodes, distinct: $distinct; left to the system: $(printf '%s\n' "$left" | grep -c .); any not reaching all $peers connections: $reached"
[ "$reached" -eq 0 ] || exit 1
case $MODE in
measured) [ "$named" -eq "$nodes" ] && [ "$distinct" -eq 1 ] && [ "$bytes" -ge 16384 ] || exit 1 ;;
set)      [ "$named" -eq "$nodes" ] && [ "$distinct" -eq 1 ] && [ "$bytes" -eq 128000 ] || exit 1 ;;
floor|fixed)
          [ "$named" -eq "$nodes" ] && [ "$distinct" -eq 1 ] && [ "$bytes" -eq 16384 ] || exit 1 ;;
by-hand)  [ "$named" -eq "$nodes" ] && [ "$bytes" -eq 65536 ] &&
              printf '%s\n' "$out" | grep -q "to 65536 (OSOE_WIRE_UNSENT_BYTES)" || exit 1 ;;
none)     [ "$named" -eq 0 ] && [ "$(printf '%s\n' "$left" | grep -c .)" -eq "$nodes" ] || exit 1 ;;
one-node) [ "$named" -eq 0 ] && [ -z "$left" ] || exit 1 ;;
esac
echo "UNSENT OK"
