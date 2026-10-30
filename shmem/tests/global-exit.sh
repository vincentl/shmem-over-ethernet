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
# so that it ends the job at the first bad exit as srun -K1 does: the PE
# carrying the status leaves last (abort.c), so its exit, the one bad one,
# must find no rank for the launcher to kill, and finish - every line and, in
# slow-handler, its handler's written - and the launcher's status must be the
# carrier's. In two-callers the PE after CALLER calls too, with STATUS + 1, and
# either may carry; the other leaves with 0. In two-nodes it is on another node
# and calls 100 ms later, CALLER's node holding its goodbye to it half a second
# (OSOE_TEST_HOLD_GOODBYE, set here): both win their nodes' words, and CALLER,
# ranked lower, must carry - the job's single carrier (P1) - its node telling
# the other though that node's goodbye arrived first. In all-callers every PE
# calls, with STATUS + its rank: one alone leaves nonzero, with its own, and
# every handler that began - a second long - finishes. A non-lead's fatal must name its
# caller on every node, which only its lead's abortive goodbye can carry
# there. The job must end within 6 s of the caller's end, before the grace,
# 10 s, but where MODE holds stdout and the grace is what ends it.
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
case $MODE in held-stdout|caller-held-stdout|held-other-node) flushes=no ;; esac
# two-nodes: the caller's node holds its goodbye to the second caller's node.
if [ "$MODE" = two-nodes ]; then
    second=$(( (CALLER + 1) % RANKS ))
    from=$(printf '%s\n' "$MAP" | cut -d, -f$((CALLER + 1)))
    to=$(printf '%s\n' "$MAP" | cut -d, -f$((second + 1)))
    OSOE_TEST_HOLD_GOODBYE=$from:$to
    export OSOE_TEST_HOLD_GOODBYE
fi
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
# Two callers: the one whose declaration won carries its status - in two-nodes
# the caller, ranked lower, the job's single carrier (P1).
expected=$STATUS
if [ "$MODE" = two-callers ]; then
    second=$(( (CALLER + 1) % RANKS ))
    second_status=$((STATUS + 1))
    second_left=$(printf '%s\n' "$out" | grep -c "rank $second exited with status $second_status\$")
    [ "$second_left" -eq 1 ] && caller_left=1 && expected=$second_status
fi
# Every PE a caller: one of them carries, with its own status.
if [ "$MODE" = all-callers ]; then
    caller_left=0
    expected=-1
    for r in $(seq 0 $((RANKS - 1))); do
        if printf '%s\n' "$out" | grep -q "rank $r exited with status $((STATUS + r))\$"; then
            caller_left=$((caller_left + 1))
            expected=$((STATUS + r))
        fi
    done
fi
written=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: computing\$")
killed=$(printf '%s\n' "$out" | grep -c "killed by signal")
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
# reset-stream: the caller's node ends by its global exit, and the other node's
# lead dies of a fatal of its own 150 ms later, ending - where input is unread,
# resetting - the connections the caller's node still reads and acknowledges
# on while its helpers wait out OSOE_TEST_ABORT_DELAY_MS. The caller's node must
# excuse that end, as a node ending anyway, and send nothing more on a
# connection that failed (progress.c's receive_wire and flush;
# abort_owners_model.py): its ranks leave with the global exit's statuses, well
# before the grace, and no line says that a connection failed or ended without
# its goodbye. The other node's ranks leave as its lead's fatal has them.
if [ "$MODE" = reset-stream ]; then
    node=$(printf '%s\n' "$MAP" | cut -d, -f$((CALLER + 1)))
    mates_left=0
    for r in $(printf '%s\n' "$MAP" | tr , '\n' | grep -n "^$node\$" | cut -d: -f1); do
        rank=$((r - 1))
        [ "$rank" -eq "$CALLER" ] && continue
        printf '%s\n' "$out" | grep -q "rank $rank \(exited\|killed\)" && mates_left=$((mates_left + 1))
    done
    failures=$(printf '%s\n' "$out" | grep -c "to node [0-9]* failed\|without its goodbye")
    echo "reset-stream: the caller left with $STATUS: $caller_left; its node's other ranks" \
         "nonzero or killed: $mates_left; $failures lines of a failed or ended connection"
    [ "$stuck" -eq 0 ] && [ "$caller_left" -eq 1 ] && [ "$mates_left" -eq 0 ] &&
        [ "$failures" -eq 0 ] || exit 1
    [ -n "$began" ] && awk -v t="$took" 'BEGIN { exit !(t < 6) }' || exit 1
    echo "GLOBAL EXIT OK"
    exit 0
fi
[ "$stuck" -eq 0 ] || exit 1
[ "$launched" -eq "$expected" ] || exit 1
if [ "$HOW" = fatal ]; then
    [ "$with_status" -eq "$RANKS" ] && [ "$killed" -eq 0 ] || exit 1
    if [ "$speaks" = yes ]; then
        [ "$strays" -eq 0 ] && [ "$lost" -eq 0 ] || exit 1
    fi
else
    # The carrier alone leaves nonzero, with its status - a status of 0 leaves
    # no line - and nobody is killed: under -k by nothing, and without it by
    # the launcher, since the carrier's exit, the one bad one, comes last.
    if [ "$STATUS" -eq 0 ]; then
        [ "$nonzero" -eq 0 ] || exit 1
    else
        [ "$nonzero" -eq 1 ] && [ "$caller_left" -eq 1 ] || exit 1
    fi
    [ "$killed" -eq 0 ] || exit 1
fi
# Every line written, but for a fatal, which flushes nothing, and for an exit
# straight after init, which other PEs may meet before their own init returns
# - though not the caller, whose own line its agent's exit flushes.
if [ "$flushes" = yes ]; then
    [ "$written" -eq "$RANKS" ] || exit 1
elif [ "$HOW" != fatal ]; then
    [ "$written" -ge 1 ] || exit 1
fi
# Ended well before the grace (abort.h, 10 s), at which an end left waiting
# would come instead - a main thread inside exit, in start-pes and returned; a
# PE whose end is late, which a launcher ending the step at the first bad exit
# would kill and report (the adversarial read of 2026-10-02: a mutant ending
# every other PE 8 s late passed eight of ten of these). Where the grace is
# what ends the job, at it and not before: otherwise the grace's own status was
# never what the statuses above saw.
case $MODE in
held-stdout|caller-held-stdout|held-other-node)
    [ -n "$began" ] && awk -v t="$took" 'BEGIN { exit !(t >= 9) }' || exit 1 ;;
*)
    [ -n "$began" ] && awk -v t="$took" 'BEGIN { exit !(t < 6) }' || exit 1 ;;
esac
case $MODE in
handlers)
    # exit on the caller's PE only, which runs its handler; _exit elsewhere.
    ran=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: handler ran\$")
    echo "handlers: $ran ran"
    [ "$ran" -eq 1 ] && printf '%s\n' "$out" | grep -q "^PE $CALLER: handler ran\$" || exit 1 ;;
slow-handler)
    # The caller's handler takes a second, and the caller must finish it.
    printf '%s\n' "$out" | grep -q "^PE $CALLER: handler ran after a second\$" || exit 1 ;;
two-nodes)
    # The outranked caller's handler finishes, before its node's lead ends its
    # stream and so before the carrier, which has none, leaves.
    second=$(( (CALLER + 1) % RANKS ))
    printf '%s\n' "$out" | grep -q "^PE $second: handler ran after a second\$" || exit 1 ;;
all-callers)
    # Every handler that began finishes, none killed inside it by a launcher
    # ending the step at an earlier bad exit; the carrier's among them.
    handlers_began=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: handler began\$")
    ran=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: handler ran after a second\$")
    echo "all-callers: $ran of the $handlers_began handlers that began finished"
    [ "$ran" -ge 1 ] && [ "$ran" -eq "$handlers_began" ] || exit 1 ;;
esac
echo "GLOBAL EXIT OK"
