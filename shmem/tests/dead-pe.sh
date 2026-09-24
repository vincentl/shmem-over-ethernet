#!/bin/sh
# dead-pe.sh LAUNCHER PROGRAM RANKS MAP DIES
#
# Runs test-dead-pe.c's job on MAP with world rank DIES killing itself, and
# requires the rank that died to be the only one killed by a signal and every
# other to have left with the failure status - none killed by the launcher at
# its timeout, none still computing or out of a barrier the dead PE never
# reached. Pidfd liveness (liveness.h) is what finds the death. -k leaves the
# survivors to the library.
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 DIES=$5
out=$("$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 -k -- "$PROGRAM" "$DIES" 2>&1)
echo "$out"
failed=$(printf '%s\n' "$out" | grep -c "exited with status 1\$")
killed=$(printf '%s\n' "$out" | grep -c "killed by signal")
bad=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|BARRIER RETURNED\|timeout after")
echo "dead PE: $killed killed, $failed of $((RANKS - 1)) survivors left with the failure" \
     "status, $bad bad"
[ "$killed" -eq 1 ] || exit 1
[ "$failed" -eq $((RANKS - 1)) ] || exit 1
[ "$bad" -eq 0 ] || exit 1
echo "DEAD PE OK"
