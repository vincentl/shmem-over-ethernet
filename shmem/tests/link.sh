#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# The link the large collectives' shapes are chosen from (v0.2 step 2;
# link-measure.h), as every PE's SHMEM_DEBUG line at start-up shows it.
#
#   link.sh LAUNCHER PROGRAM MAP measured|few-lanes|set|fixed|none
#
#   measured  the root's lead measures it at the end of shmem_init and every
#             PE holds the root's numbers: one line a PE, every one naming
#             the same hop, message cost and bandwidth, the hop and the
#             bandwidth above zero; and under OSOE_TEST_PRINT_LINK=1 (T19),
#             which every launch here sets as every multi-node test does, one
#             line more, from rank 0, giving the same three numbers as the
#             OSOE_LINK_* settings that pin them.
#   few-lanes measured, with the second PE - node 1's first, on a map that
#             starts 0,1 - at OSOE_MAX_THREADS=1 and every lane as small as
#             the settings make it: the measurement's streams stay within
#             the fewest lanes a node can have, whatever the root's own
#             OSOE_MAX_THREADS, which nodes need not agree on (the
#             adversarial read of Part 1, finding 1: they ran past node 1's
#             region, and the job ended at start naming the wrong settings).
#   set       OSOE_LINK_* give it by hand: every PE names exactly those
#             numbers, and nothing is measured, nor printed as measured.
#   fixed     OSOE_COLLECTIVE_SHAPE=fixed: nothing chooses, so no PE names a
#             link.
#   none      a job of one node: no PE names a link.
#
# PROGRAM is any program that initializes, once, and finishes.
set -u
LAUNCHER=$1 PROGRAM=$2 MAP=$3 MODE=$4
export OSOE_TEST_PRINT_LINK=1
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
case $MODE in
set)
    out=$(SHMEM_DEBUG=1 OSOE_LINK_HOP_NS=50000 OSOE_LINK_MESSAGE_NS=7000 \
        OSOE_LINK_BYTES_PER_SECOND=116000000 \
        "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
fixed)
    out=$(SHMEM_DEBUG=1 OSOE_COLLECTIVE_SHAPE=fixed \
        "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
few-lanes)
    out=$(SHMEM_DEBUG=1 OSOE_COLLECTIVE_FANOUT=2 OSOE_COLLECTIVE_PIPELINE=1 \
        "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -e 1:OSOE_MAX_THREADS=1 -- "$PROGRAM" 2>&1) ;;
*)
    out=$(SHMEM_DEBUG=1 "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1) ;;
esac
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
lines=$(printf '%s\n' "$out" | grep -c '^osoe: PE [0-9]* of [0-9]* (local')
[ "$lines" -eq "$ranks" ] || { echo "$lines start-up lines for $ranks PEs"; exit 1; }
# What each PE's line says of the link, less what establishing it cost that
# PE, which is its own.
links=$(printf '%s\n' "$out" | grep '^osoe: PE [0-9]* of [0-9]* (local' |
        sed -n 's/.*; link \(.*\)$/\1/p' | sed 's/ ([0-9]* us of this start)$//')
named=$(printf '%s\n' "$links" | grep -c .)
distinct=$(printf '%s\n' "$links" | grep . | sort -u | wc -l | tr -d ' ')
echo "PEs naming a link: $named of $ranks; distinct: $distinct"
# OSOE_TEST_PRINT_LINK's line, rank 0's alone.
said='^osoe: the link measured at start-up, to run again on it: '
printed=$(printf '%s\n' "$out" | grep -c "$said")
echo "lines giving the measured link as settings: $printed"
case $MODE in
measured|few-lanes)
    [ "$named" -eq "$ranks" ] && [ "$distinct" -eq 1 ] || exit 1
    one=$(printf '%s\n' "$links" | head -1)
    hop=$(printf '%s\n' "$one" | sed -n 's/^measured: a hop of \([0-9]*\) ns, .*/\1/p')
    bandwidth=$(printf '%s\n' "$one" | sed -n 's/.*ns, \([0-9]*\) bytes a second$/\1/p')
    echo "measured: hop ${hop:-?} ns, ${bandwidth:-?} bytes a second"
    # Within what a setting may give (bootstrap.h): a PE left holding the
    # all-ones every other PE contributes - no root's numbers reaching it - is
    # past both, and too long a number for the shell, which refuses it.
    [ -n "$hop" ] && [ -n "$bandwidth" ] && [ "$hop" -gt 0 ] && [ "$bandwidth" -gt 0 ] &&
        [ "$hop" -le 10000000000 ] && [ "$bandwidth" -le 10000000000000 ] || exit 1
    message=$(printf '%s\n' "$one" | sed -n 's/.*, a message cost of \([0-9]*\) ns,.*/\1/p')
    settings="OSOE_LINK_HOP_NS=$hop OSOE_LINK_MESSAGE_NS=$message"
    settings="$settings OSOE_LINK_BYTES_PER_SECOND=$bandwidth"
    [ "$printed" -eq 1 ] || exit 1
    line=$(printf '%s\n' "$out" | grep "$said" | sed "s/$said//")
    echo "printed: $line"
    [ "$line" = "$settings" ] || { echo "the start-up lines say $settings"; exit 1; }
    ;;
set)
    [ "$named" -eq "$ranks" ] && [ "$distinct" -eq 1 ] && [ "$printed" -eq 0 ] || exit 1
    expected="set: a hop of 50000 ns, a message cost of 7000 ns, 116000000 bytes a second"
    [ "$(printf '%s\n' "$links" | head -1)" = "$expected" ] || exit 1
    ;;
fixed|none)
    [ "$named" -eq 0 ] && [ "$printed" -eq 0 ] || exit 1
    ;;
esac
echo "LINK OK"
