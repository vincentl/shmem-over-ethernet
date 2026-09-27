#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# global-exit.sh LAUNCHER PROGRAM RANKS MAP HOW STATUS [CALLER [MODE]]
#
# Runs test-global-exit.c's job - PE CALLER calls shmem_global_exit(HOW), or
# with HOW=fatal ends the job with a fatal, while every other PE computes or
# streams - and requires every rank to have left with STATUS, none to have
# been killed or to have outlasted its computing, and, for a global exit, every
# rank's unflushed line to have been written by exit's flush (§9.1.6). MAP is
# the node map, or - for one node. CALLER (0) and MODE (compute) are
# test-global-exit.c's: which PE ends the job, and when and beside what. -k
# leaves the survivors to the library. A non-lead's fatal must name its caller
# on every node, which only its lead's abortive goodbye can carry there. Where MODE puts a main
# thread inside exit, the job must end within 6 s of the caller's end: exit's
# lock then left waiting would end it only at the grace, 10 s.
# WRAP, if set, is a command the launcher runs under: tier 3 runs it through
# tools/launch/tier3.sh, so that each node has a network namespace and a shaped
# link of its own (shmem/tests/CMakeLists.txt).
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 HOW=$5 STATUS=$6 CALLER=${7:-0} MODE=${8:-compute}
# A fatal, or an end straight after init, flushes nothing a PE had not written.
flushes=yes
[ "$HOW" = fatal ] || [ "$MODE" = immediate ] && flushes=no
if [ "$MAP" = - ]; then
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -t 60 -k -- "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
else
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 -k -- \
          "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
fi
ended=$(date +%s.%N)
echo "$out"
exited=$(printf '%s\n' "$out" | grep -c "exited with status $STATUS\$")
written=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: computing\$")
bad=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|killed by signal\|timeout after")
# A fatal's goodbye names the caller on every other node; lost, those nodes
# end with the failure too, but naming a PE of their own. Only where the
# caller's lead's agent speaks for it: a lead's own fatal ends at once, telling
# nobody (§3.15), and the other nodes end on its end of stream.
speaks=yes
if [ "$MAP" != - ]; then
    node=$(printf '%s\n' "$MAP" | cut -d, -f$((CALLER + 1)))
    first=$(printf '%s\n' "$MAP" | tr , '\n' | grep -n "^$node\$" | head -1 | cut -d: -f1)
    [ "$first" -eq $((CALLER + 1)) ] && speaks=no
fi
strays=$(printf '%s\n' "$out" | grep -o "on behalf of PE [0-9]*" | grep -vc "PE $CALLER\$")
lost=$(printf '%s\n' "$out" | grep -c "without its goodbye")
began=$(printf '%s\n' "$out" | sed -n 's/^ending the job at \([0-9.]*\)$/\1/p' | head -1)
took=$(echo "${began:-0} $ended" | awk '{ printf "%.1f", $2 - $1 }')
echo "global exit: $exited of $RANKS ranks left with status $STATUS; $written of $RANKS" \
     "lines written; $bad ranks killed or still computing; $strays ends naming another" \
     "PE, $lost goodbyes lost; ended ${took} s after the caller"
[ "$exited" -eq "$RANKS" ] || exit 1
[ "$bad" -eq 0 ] || exit 1
if [ "$HOW" = fatal ] && [ "$speaks" = yes ]; then
    [ "$strays" -eq 0 ] && [ "$lost" -eq 0 ] || exit 1
fi
# Every line written, but for a fatal, which flushes nothing, and for an exit
# straight after init, which other PEs may meet before their own init returns
# - though not the caller, whose own line its agent's exit flushes.
if [ "$flushes" = yes ]; then
    [ "$written" -eq "$RANKS" ] || exit 1
elif [ "$HOW" != fatal ]; then
    [ "$written" -ge 1 ] || exit 1
fi
# Where a main thread is inside exit, ended well before the grace (abort.h,
# 10 s), at which an exit left waiting on it would end instead.
case $MODE in
start-pes|returned)
    [ -n "$began" ] && awk -v t="$took" 'BEGIN { exit !(t < 6) }' || exit 1 ;;
handlers)
    # exit on the caller's PE only, which runs its handler; _exit elsewhere.
    ran=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: handler ran\$")
    echo "handlers: $ran ran"
    [ "$ran" -eq 1 ] && printf '%s\n' "$out" | grep -q "^PE $CALLER: handler ran\$" || exit 1 ;;
esac
echo "GLOBAL EXIT OK"
