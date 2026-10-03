#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Run inside the VM. $1 = private tree root, $2 = dir with the built programs.
T=${1:?}; O=${2:?}; L=$T/build/debug/tools/launch/osoe-launch
snapshot() { echo "  /dev/shm: $(ls /dev/shm | grep -c "^osoe\.$1\.") objects, $(du -ck /dev/shm/osoe.$1.* 2>/dev/null | tail -1 | cut -f1) KiB resident; sleeper processes: $(pgrep -f "$O/sleeper" | wc -l)"; }

echo "## A: SIGTERM to the launcher mid-job (what ctest's timeout, or kill, does)"
$L -n 4 -m 0,0,1,1 -t 120 -- $O/sleeper > /tmp/omx-a.out 2>&1 & LP=$!
sleep 3; echo "  before:"; snapshot $LP
kill -TERM $LP; sleep 3; echo "  3 s after SIGTERM to launcher pid $LP:"; snapshot $LP
sleep 32; echo "  35 s after (PEs have finished their sleep and finalized on their own):"; snapshot $LP
pkill -f "$O/sleeper"; rm -f /dev/shm/osoe.$LP.*

echo "## B: SIGTERM to every PE at once (what scancel does to a step), launcher told to keep hands off (-k)"
OSOE_WATCHDOG=0 $L -n 4 -m 0,0,1,1 -k -t 20 -- $O/sleeper > /tmp/omx-b.out 2>&1 & LP=$!
sleep 3; pkill -TERM -f "$O/sleeper"; sleep 1
echo "  1 s after SIGTERM to all PEs:"; snapshot $LP
ls -la /dev/shm | grep "osoe\.$LP\." | awk '{print "    " $5, $9}'
wait $LP; echo "  launcher rc=$?"; tail -3 /tmp/omx-b.out | sed 's/^/    /'

echo "## C: one PE leaves with exit(0) and no shmem_finalize; watchdog off (the release default), -k"
OSOE_WATCHDOG=0 $L -n 4 -m 0,0,1,1 -k -t 15 -- $O/sleeper 3 exit > /tmp/omx-c.out 2>&1; echo "  launcher rc=$?"; sed 's/^/    /' /tmp/omx-c.out | tail -4
echo "## C2: the same with the development watchdog on"
OSOE_WATCHDOG=1 $L -n 4 -m 0,0,1,1 -k -t 15 -- $O/sleeper 3 exit > /tmp/omx-c2.out 2>&1; echo "  launcher rc=$?"; sed 's/^/    /' /tmp/omx-c2.out | tail -6
echo "## D: fork(), system(), popen() inside a PE, child leaves with exit()"
$L -n 2 -t 30 -- $O/fork-in-pe; echo "  rc=$?"
echo "## D2: the same under start_pes (an atexit finalize is registered, and the child runs it)"
$L -n 2 -t 30 -- $O/fork-in-pe start_pes; echo "  rc=$?"
