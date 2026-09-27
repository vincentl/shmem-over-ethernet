#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# vanished-node.sh LAUNCHER PROGRAM RANKS MAP
#
# Tier 3 (tools/launch/tier3.sh, as WRAP): runs test-vanished-node.c's job on
# MAP - two nodes, the second's ranks the last half - with keepalive at its
# tightest, cuts the last node's link once the job is ready, and requires the
# job to end with a fatal saying a node stopped answering and every rank to
# leave with the failure status: none killed by the launcher at its timeout,
# still computing, or out of the barrier. -k leaves the ranks to the library.
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4
out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 -k -- "$PROGRAM" 2>&1)
echo "$out"
failed=$(printf '%s\n' "$out" | grep -c "exited with status 1\$")
found=$(printf '%s\n' "$out" | grep -c "stopped answering")
bad=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|BARRIER RETURNED\|timeout after\|killed by signal")
echo "vanished node: found $found times, $failed of $RANKS ranks left with the failure" \
     "status, $bad bad"
[ "$found" -ge 1 ] || exit 1
[ "$failed" -eq "$RANKS" ] || exit 1
[ "$bad" -eq 0 ] || exit 1
echo "VANISHED NODE OK"
