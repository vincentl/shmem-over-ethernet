#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# A job never outlives its launcher (launch.c; review round two, omissions 6).
#
#   launcher-signals.sh LAUNCHER PROGRAM term|int|kill [one-node]
#
# Starts a four-PE, two-node job of PROGRAM (sleeper.c) - one node with
# `one-node`, which both builds run, the launcher being the desktop's way to
# run a job (v0.2 step 1) - waits until every PE has initialized, then sends
# the launcher SIGTERM, SIGINT or SIGKILL. Within ten seconds every PE must be
# gone, and no object of the job may have a name.
#   term, int  the launcher forwards the signal and exits 128 + it. The PEs
#              must die of the forwarded signal itself, not of the SIGKILL that
#              follows two seconds later: that is what tells a forward from a
#              timeout. SIGINT is the case a shell's background job starts with
#              ignored, and the children must not inherit that.
#   kill       the launcher has no chance to do anything; PR_SET_PDEATHSIG does.
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3 NODES=${4:-two}
out=$(mktemp)
trap 'rm -f "$out"' EXIT

map="-m 0,0,1,1"
[ "$NODES" = one-node ] && map=""
# shellcheck disable=SC2086 # $map is two words or none
"$LAUNCHER" -n 4 $map -t 120 -- "$PROGRAM" > "$out" 2>&1 &
LP=$!
i=0
while [ "$(grep -c '^running' "$out")" -lt 4 ]; do
    i=$((i + 1))
    if [ $i -gt 300 ]; then
        cat "$out"; echo "the job never started"; kill -KILL $LP; exit 1
    fi
    sleep 0.1
done
pes=$(pgrep -P $LP)
echo "PEs: $(echo $pes)"

case $MODE in
term) kill -TERM $LP ;;
int)  kill -INT $LP ;;
kill) kill -KILL $LP ;;
esac

# A PE is gone when its /proc entry is, or is a zombie awaiting its reaper.
alive() {
    n=0
    for pe in $pes; do
        state=$(sed -n 's/^State:[[:space:]]*\([A-Z]\).*/\1/p' /proc/$pe/status 2>/dev/null)
        [ -n "$state" ] && [ "$state" != Z ] && n=$((n + 1))
    done
    echo $n
}
i=0
while [ "$(alive)" -gt 0 ] && [ $i -lt 100 ]; do
    i=$((i + 1)); sleep 0.1
done

if [ "$MODE" != kill ]; then
    wait $LP
    echo "launcher exited $?"
fi
cat "$out"
echo "escalated to SIGKILL: $(grep -c 'killed by signal 9' "$out")"
echo "PEs still running: $(alive)"
echo "objects of job $LP with a name: $(ls /dev/shm | grep -c "^osoe\.$LP\.")"
