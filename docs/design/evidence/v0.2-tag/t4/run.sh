#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# run.sh: T4 of the 0.2 tag's checks, detached in the osoe VM: the tag tree's release build, SOS at four
# logical nodes of two PEs at OSOE_PROGRESS_HELPERS=0 and 2, and the team programs at 70 PEs at 0 and 2,
# every build and run under the VM's lock, held from the first build to the last run. A sampler beside
# each phase records what else was using the machine. Progress in progress.log, ending in a DONE line.
#   setsid nohup bash run.sh > driver.log 2>&1 < /dev/null &
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/progress.log
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }
: > "$LOG"
say "T4 starts (pid $$); waiting for the VM's lock; load $(cut -d' ' -f1-3 /proc/loadavg)"
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || { say "NO LOCK after 10800 s"; echo "DONE no-lock" >> "$LOG"; exit 99; }
say "lock held; load $(cut -d' ' -f1-3 /proc/loadavg); memory: $(free -m | awk '/Mem:/ {print $4 " MiB free, " $7 " MiB available"}'); /dev/shm: $(ls /dev/shm | wc -l) entries"
{ echo "-- the busiest processes as the lock was taken:"; ps -eo pid,etime,pcpu,args --sort=-pcpu | head -8 | cut -c1-160; } >> "$LOG"
strays() {   # processes whose working directory is under /var/tmp/t4-, other than this driver's own
  local p c found=0
  for p in /proc/[0-9]*; do
    c=$(readlink "$p/cwd" 2>/dev/null) || continue
    case $c in /var/tmp/t4-*) [ "${p#/proc/}" = "$$" ] || { found=1; echo "stray ${p#/proc/}: $c: $(tr '\0' ' ' < "$p/cmdline" | cut -c1-100)"; } ;; esac
  done
  return $found
}
phase() {   # NAME COMMAND...
  local name=$1; shift
  : > "$HERE/load-$name.log"
  python3 -I "$HERE/load-sampler.py" $$ 10 "$HERE/load-$name.log" 9>&- &
  local sampler=$!
  say "phase $name begins; load $(cut -d' ' -f1-3 /proc/loadavg)"
  "$@" > "$HERE/phase-$name.log" 2>&1
  local rc=$?
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  say "phase $name ends rc=$rc; load $(cut -d' ' -f1-3 /proc/loadavg)"
  strays | while read -r line; do say "$line"; done
  return $rc
}
phase build bash "$HERE/build.sh" || { say "build failed; stopping"; echo "DONE build-failed" >> "$LOG"; exit 1; }
phase sos-h0 bash "$HERE/sos-sweep.sh" 0
phase sos-h2 bash "$HERE/sos-sweep.sh" 2
phase teams-h0 bash "$HERE/teams70.sh" 0
phase teams-h2 bash "$HERE/teams70.sh" 2
say "all phases ended"
echo "DONE" >> "$LOG"
