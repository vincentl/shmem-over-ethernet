#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# run-experiment.sh NAME MAP COUNT TIMEOUT SETTLE -- hang-arguments...
# Launches the hang program under osoe-launch, waits SETTLE seconds, then acts
# as an operator with osoe-stat alone, logging the commands and their output.
. <repo>/build/scratch/review-diagnosability/env.sh
name=$1 map=$2 count=$3 timeout=$4 settle=$5; shift 6
log=$E/logs/$name.log
: > $log
: > $E/logs/$name.dump.log
say() { echo "\$ $*" >> $log; "$@" >> $log 2>&1; echo >> $log; }
$LAUNCH -n $count -m $map -t $timeout ${LAUNCH_FLAGS:-} -- $W/hang "$@" > $E/logs/$name.job.log 2>&1 &
launcher=$!
sleep $settle
[ -n "$HOOK" ] && { echo "# hook: $HOOK" >> $log; eval "$HOOK" >> $log 2>&1; sleep ${HOOK_SETTLE:-5}; }
echo "# job id (launcher pid) $launcher" >> $log
say $STAT --list
for seg in /dev/shm/osoe.$launcher.*; do
    case $seg in *heap*|*conn*|*wire*|*job*) continue;; esac
    say $STAT --counters --nonzero $seg
done
sleep 3
for seg in /dev/shm/osoe.$launcher.*; do
    case $seg in *heap*|*conn*|*wire*|*job*) continue;; esac
    say $STAT --counters --nonzero $seg
    say $STAT --threads $seg
    echo "\$ osoe-stat --dump $seg" >> $E/logs/$name.dump.log
    $STAT --dump $seg >> $E/logs/$name.dump.log 2>&1
done
ls -la /dev/shm | grep "osoe.$launcher" >> $log
[ -n "$HOOK2" ] && { echo "# hook2: $HOOK2" >> $log; eval "$HOOK2" >> $log 2>&1; }
wait $launcher
echo "# launcher exit $?" >> $log
echo "# left in /dev/shm afterwards:" >> $log
ls -la /dev/shm | grep "osoe.$launcher" >> $log
