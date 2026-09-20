#!/bin/sh
T=${1:?}; E=$(cd $(dirname "$0") && pwd); L=$T/build/debug/tools/launch/osoe-launch
gcc -std=c11 -O1 -g -I$T/shmem/include $E/fatal-under-start-pes.c -o /tmp/omx/fatal-under-start-pes -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem || exit 1
for w in 0 1; do for mode in start_pes init; do
echo "## $mode, OSOE_WATCHDOG=$w, launcher -k (hands off), 3 PEs"
OSOE_WATCHDOG=$w $L -n 3 -k -t 12 -- /tmp/omx/fatal-under-start-pes $mode 2>&1 | sed 's/^/  /'
done; done
