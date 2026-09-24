#!/bin/sh
# death-in-wait.sh LAUNCHER PROGRAM RANKS MAP WAIT ROUTINE
#
# Runs test-death-in-wait.c: the last rank dies while every other waits in WAIT
# (barrier or wait_until) - after a put to the PE half the job away, so that on
# two nodes the barrier holds a quiet of its own inside its wait - and requires
# every survivor to end naming ROUTINE, from its agent or from its wait's own
# sweep (design §3.15), and none to have been killed at the launcher's timeout
# or to have returned from the wait. MAP is the node map, or - for one node.
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 WAIT=$5 ROUTINE=$6
set -- -n "$RANKS" -t 20 -k
[ "$MAP" = - ] || set -- "$@" -m "$MAP"
r=0
while [ "$r" -lt "$RANKS" ]; do
    set -- "$@" -e "$r:OSOE_TEST_WAIT=$WAIT"
    r=$((r + 1))
done
out=$("$LAUNCHER" "$@" -- "$PROGRAM" 2>&1)
echo "$out"
named=$(printf '%s\n' "$out" |
        grep -E "$ROUTINE: the job is aborting on behalf of PE|waiting in $ROUTINE\$" |
        sed -n 's/^osoe: PE \([0-9]*\):.*/\1/p' | sort -u | wc -l)
bad=$(printf '%s\n' "$out" | grep -c "timeout after\|returned from a wait")
want=$((RANKS - 1))
echo "death in a wait: $named of $want survivors named $ROUTINE; $bad timed out or returned"
[ "$named" -eq "$want" ] && [ "$bad" -eq 0 ] || exit 1
echo "DEATH IN WAIT OK"
