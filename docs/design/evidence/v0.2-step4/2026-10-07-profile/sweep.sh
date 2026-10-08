#!/bin/bash
# v0.2 step 4 part 0: the put stream against the connection buffer's depth -
# OSOE_AM_BUFFER and OSOE_AM_CHUNK by the environment alone. Takes the VM lock.
# usage: sweep.sh NAME OUT
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
NAME=$1; OUT=$2; mkdir -p $OUT
R=/tmp/ladder/$NAME/build/release
L=$R/tools/launch/osoe-launch
W=/tmp/ladder/$NAME/work
[ -x $W/putstream ] || { echo "no putstream in $W"; exit 1; }
: > $OUT/sweep.txt
for round in 1 2; do
  for cfg in "131072 65536" "131072 32768" "131072 16384" "262144 65536" "524288 65536" "1048576 65536"; do
    set -- $cfg
    r=$(env OSOE_AM_BUFFER=$1 OSOE_AM_CHUNK=$2 $L -n 2 -m 0,1 -t 60 -- $W/putstream 65536 10 2>&1 | grep '^putstream')
    echo "round=$round buffer=$1 chunk=$2 $r" | tee -a $OUT/sweep.txt
  done
done
echo DONE
