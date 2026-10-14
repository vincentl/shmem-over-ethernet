#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# dead-pe.sh LAUNCHER PROGRAM RANKS MAP DIES
#
# Runs test-dead-pe.c's job on MAP with world rank DIES killing itself, and
# requires the rank that died to be the only one killed by a signal and every
# other to have left with the failure status - none killed by the launcher at
# its timeout, none still computing or out of a barrier the dead PE never
# reached. Pidfd liveness (liveness.h) is what finds the death. -k leaves the
# survivors to the library.
# WRAP, when set, is a program the launcher runs under - deny-pidfd, so that
# liveness watches by pid as on a kernel before Linux 5.3. ONE_LINE, when set,
# requires every rank to have written one line of the library's at most: an end
# is said once a process, its cause or its "ending", and a fatal after start-up
# advises osoe-stat on no segment its job's end takes with it (D18, D20 to
# D22).
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 DIES=$5
out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 -k -- "$PROGRAM" "$DIES" 2>&1)
echo "$out"
failed=$(printf '%s\n' "$out" | grep -c "exited with status 1\$")
killed=$(printf '%s\n' "$out" | grep -c "killed by signal")
bad=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|BARRIER RETURNED\|timeout after")
echo "dead PE: $killed killed, $failed of $((RANKS - 1)) survivors left with the failure" \
     "status, $bad bad"
[ "$killed" -eq 1 ] || exit 1
[ "$failed" -eq $((RANKS - 1)) ] || exit 1
[ "$bad" -eq 0 ] || exit 1
if [ -n "${ONE_LINE:-}" ]; then
    many=$(printf '%s\n' "$out" | sed -n 's/^osoe: PE \([0-9]*\): .*/\1/p' | sort | uniq -d |
        tr '\n' ' ')
    echo "dead PE: ranks that wrote more than one line: ${many:-none}"
    [ -z "$many" ] || exit 1
fi
echo "DEAD PE OK"
