#!/bin/sh
# What does a job killed with SIGKILL leave in /dev/shm, and how much of it does
# `osoe-stat --gc` remove? Usage: stale-segments.sh <build dir> <program that inits and sleeps>
set -u
build=$1; program=$2
setsid "$build/tools/launch/osoe-launch" -n 4 -m 0,0,1,1 -t 60 -- "$program" &
launcher=$!
sleep 3
echo "== while running:"; ls -la /dev/shm | grep "osoe\.$launcher\." | awk '{print $1, $3, $5, $9}'
pkill -KILL -s "$launcher"
sleep 1
echo "== after SIGKILL of the whole job:"; ls /dev/shm | grep "osoe\.$launcher\."
echo "== osoe-stat --gc:"; "$build/tools/stat/osoe-stat" --gc 2>&1 | grep "$launcher"
echo "== still there after --gc:"; ls /dev/shm | grep "osoe\.$launcher\."
echo "== removing the rest by hand"; rm -f /dev/shm/osoe.$launcher.*
