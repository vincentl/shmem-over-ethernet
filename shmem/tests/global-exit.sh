#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# global-exit.sh LAUNCHER PROGRAM RANKS MAP HOW STATUS [CALLER [MODE]]
#
# Runs test-global-exit.c's job - PE CALLER calls shmem_global_exit(HOW), or
# with HOW=fatal ends the job with a fatal, while every other PE computes or
# streams - and requires:
#
#   - for a global exit, the caller to have left with STATUS and every other
#     rank with 0 (abort.h: the status is the job's and the caller's, so that a
#     launcher ending the step at the first bad exit sees none before the
#     caller's); for a fatal, every rank to have left with STATUS;
#   - the launcher's own status to be STATUS, as srun's would be;
#   - no rank killed or outlasting its computing;
#   - for a global exit, every rank's unflushed line written by its flush
#     (§9.1.6).
#
# MAP is the node map, or - for one node. CALLER (0) and MODE (compute) are
# test-global-exit.c's: which PE ends the job, and when and beside what. -k
# leaves the survivors to the library. KILLS=yes runs the launcher without it,
# so that it ends the job at the first bad exit as srun -K1 does: the caller's
# exit must then be the first bad one, and finish - its line and, in
# slow-handler, its handler's written - and the launcher's status must be the
# caller's. A rank the launcher kills after the caller has left is counted and
# said, not failed: it was ending already. A non-lead's fatal must name its
# caller on every node, which only its lead's abortive goodbye can carry
# there. Where MODE puts a main thread inside exit, the job must end within
# 6 s of the caller's end: exit's lock then left waiting would end it only at
# the grace, 10 s.
# WRAP, if set, is a command the launcher runs under: tier 3 runs it through
# tools/launch/tier3.sh, so that each node has a network namespace and a shaped
# link of its own (shmem/tests/CMakeLists.txt).
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 HOW=$5 STATUS=$6 CALLER=${7:-0} MODE=${8:-compute}
KEEP=-k
[ "${KILLS:-}" = yes ] && KEEP=
# A fatal, or an end straight after init, flushes nothing a PE had not written;
# nor does an end the grace makes for a PE whose stdout is held, which skips a
# stream it cannot lock (abort.c).
flushes=yes
[ "$HOW" = fatal ] || [ "$MODE" = immediate ] && flushes=no
case $MODE in held-stdout|caller-held-stdout) flushes=no ;; esac
if [ "$MAP" = - ]; then
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -t 60 $KEEP -- "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
else
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 $KEEP -- \
          "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
fi
launched=$?
ended=$(date +%s.%N)
echo "$out"
nonzero=$(printf '%s\n' "$out" | grep -c "exited with status [0-9]*\$")
with_status=$(printf '%s\n' "$out" | grep -c "exited with status $STATUS\$")
caller_left=$(printf '%s\n' "$out" | grep -c "rank $CALLER exited with status $STATUS\$")
written=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: computing\$")
caller_written=$(printf '%s\n' "$out" | grep -c "^PE $CALLER: computing\$")
killed=$(printf '%s\n' "$out" | grep -c "killed by signal")
caller_killed=$(printf '%s\n' "$out" | grep -c "rank $CALLER killed by signal")
stuck=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|timeout after")
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
echo "global exit: $nonzero of $RANKS ranks left nonzero, $with_status with status $STATUS" \
     "(the caller's: $caller_left); the launcher exited $launched; $written of $RANKS lines" \
     "written; $killed ranks killed, $stuck still computing; $strays ends naming another" \
     "PE, $lost goodbyes lost; ended ${took} s after the caller"
[ "$stuck" -eq 0 ] || exit 1
[ "$launched" -eq "$STATUS" ] || exit 1
if [ "$HOW" = fatal ]; then
    [ "$with_status" -eq "$RANKS" ] && [ "$killed" -eq 0 ] || exit 1
    if [ "$speaks" = yes ]; then
        [ "$strays" -eq 0 ] && [ "$lost" -eq 0 ] || exit 1
    fi
elif [ -n "$KEEP" ]; then
    # The caller alone leaves nonzero, with STATUS; a status of 0 leaves no line.
    if [ "$STATUS" -eq 0 ]; then
        [ "$nonzero" -eq 0 ] || exit 1
    else
        [ "$nonzero" -eq 1 ] && [ "$caller_left" -eq 1 ] || exit 1
    fi
    [ "$killed" -eq 0 ] || exit 1
else
    # Without -k: the caller's exit is the first bad one and finishes.
    [ "$caller_killed" -eq 0 ] && [ "$nonzero" -eq 1 ] && [ "$caller_left" -eq 1 ] || exit 1
    [ "$caller_written" -eq 1 ] || exit 1
    echo "ranks still ending when the caller left, killed by the launcher: $killed"
fi
# Every line written, but for a fatal, which flushes nothing, and for an exit
# straight after init, which other PEs may meet before their own init returns
# - though not the caller, whose own line its agent's exit flushes. Without -k
# a rank the launcher kills writes nothing, and only the caller's is required.
if [ "$flushes" = yes ] && [ -n "$KEEP" ]; then
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
slow-handler)
    # The caller's handler takes a second, and the caller must finish it.
    printf '%s\n' "$out" | grep -q "^PE $CALLER: handler ran after a second\$" || exit 1 ;;
held-stdout|caller-held-stdout)
    # Ended by the grace, OSOE_ABORT_GRACE_SECONDS (10 s), and not before:
    # otherwise the grace's own status was never what the statuses above saw.
    [ -n "$began" ] && awk -v t="$took" 'BEGIN { exit !(t >= 9) }' || exit 1 ;;
esac
echo "GLOBAL EXIT OK"
