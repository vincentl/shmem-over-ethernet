#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A node's PEs in more than one PID namespace (D26 of docs/prompts/v0.2-open-items.md).
#
#   pid-namespaces.sh LAUNCHER join PROGRAM
#   pid-namespaces.sh LAUNCHER leftover KILLED PROGRAM STAT
#
# Each puts a process in a PID namespace of its own with unshare(1), inside a
# user namespace so that it needs no privilege, and skips - status 77 - where
# the system refuses one.
#   join      a job of three PEs on one node, local PE 1 in a namespace of its
#             own and local PE 2 a second late, so that PE 1 waits for the node
#             to join and looks at its lead meanwhile: the node must be refused
#             as it joins, PEs 0 and 1 named by each PE that reaches the check
#             before another's refusal ends it - and PE 1 must not first take
#             its lead for dead, as a PE that judged its lead by a pid from
#             another namespace did.
#   leftover  a job of one PE in a namespace of its own, killed after init with
#             its control segment kept by name (KILLED is test-kill-after-init.c),
#             its launcher pid 1 there and the PE pid 2 - which outside is a
#             live kernel thread. A start by the same ids outside must refuse
#             the segment as another namespace's, not as a running job's (round
#             six's shifted pids); and osoe-stat --gc must leave it alone,
#             saying why. The segment is removed at the end.
set -u
LAUNCHER=$1 MODE=$2
# --kill-child: a process the launcher's timeout ends takes what it started
# there with it, as the launcher's own children are taken (PR_SET_PDEATHSIG).
own_namespace() { unshare --user --map-root-user --pid --fork --kill-child "$@"; }
if ! own_namespace true 2> /dev/null; then
    echo "SKIP: unshare --user --map-root-user --pid --fork is refused here"
    exit 77
fi

case $MODE in
join)
    PROGRAM=$3
    # shellcheck disable=SC2016 # $0 and OSOE_LOCALID are the PE's, not this shell's
    out=$("$LAUNCHER" -n 3 -t 30 -k -- sh -c 'case $OSOE_LOCALID in
        (1) exec unshare --user --map-root-user --pid --fork --kill-child "$0" ;;
        (2) sleep 1 ;;
        esac; exec "$0"' "$PROGRAM" 2>&1)
    status=$?
    printf '%s\n' "$out"
    echo "job exited $status"
    named=$(printf '%s\n' "$out" | grep -c "the node's PEs are in more than one PID namespace: local PE 0 (rank 0) is in pid:\[[0-9]*\] and local PE 1 (rank 1) in pid:\[[0-9]*\]")
    echo "PEs refusing the node by name: $named"
    ;;
leftover)
    KILLED=$3 PROGRAM=$4 STAT=$5
    J=$((950000000 + $$ % 1000000))
    trap 'rm -f /dev/shm/osoe.$J.*' EXIT
    own_namespace env OSOE_KEEP_CONTROL_SEGMENT=1 OSOE_TEST_SIGNAL=KILL \
        "$LAUNCHER" -n 1 -t 30 -k -e 0:OSOE_JOB_ID=$J -- "$KILLED" > /dev/null 2>&1
    if [ ! -e "/dev/shm/osoe.$J.0.0.0" ]; then
        echo "the killed job left no control segment to find"
        exit 1
    fi
    out=$("$LAUNCHER" -n 1 -t 30 -e 0:OSOE_JOB_ID=$J -- "$PROGRAM" 2>&1)
    status=$?
    printf '%s\n' "$out"
    echo "the start by the same ids exited $status"
    "$STAT" --gc "/osoe.$J.0.0.0"
    if [ -e "/dev/shm/osoe.$J.0.0.0" ]; then
        echo "the segment is left after --gc"
    fi
    ;;
*)
    echo "pid-namespaces.sh: no mode $MODE"
    exit 2
    ;;
esac
