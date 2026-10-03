#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
T=${1:?}; E=$(cd $(dirname "$0") && pwd); L=$T/build/debug/tools/launch/osoe-launch
gcc -std=c11 -O1 -g -I$T/shmem/include $E/core-size.c -o /tmp/omx/core-size -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem || exit 1
echo "## core file of one PE of two, each with 64 MiB of heap touched (coredump_filter=$(cat /proc/self/coredump_filter), core_pattern=$(cat /proc/sys/kernel/core_pattern))"
rm -rf /tmp/omx/cores; mkdir -p /tmp/omx/cores; cd /tmp/omx/cores
( ulimit -c unlimited; $L -n 2 -t 20 -- /tmp/omx/core-size 2>&1 | head -3 )
ls -la /tmp/omx/cores | awk 'NR>1 && $9 !~ /^\.+$/ {printf "  %s: %d MiB apparent\n", $9, $5/1048576}'; du -sm /tmp/omx/cores | awk '{print "  on disk: " $1 " MiB"}'
rm -rf /tmp/omx/cores

echo "## name squatting: another user pre-creates the control segment name of a predictable job id"
J=424242; N=/dev/shm/osoe.$J.0.0.0
sudo -u nobody sh -c "umask 0; : > $N"; ls -la $N
env OSOE_PROCID=0 OSOE_STEP_NUM_TASKS=1 OSOE_NODEID=0 OSOE_STEP_NUM_NODES=1 OSOE_LOCALID=0 OSOE_STEP_ID=0 OSOE_JOB_ID=$J OSOE_GTIDS=0 /tmp/omx/env-size; echo "  PE exit status: $?"
sudo rm -f $N
