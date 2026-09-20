#!/bin/sh
# CPU consumed by a job whose PEs are all asleep in the application (sleep(30)):
# what the library's own threads cost a 4-core node. Measured under contention.
T=${1:?}; O=${2:?}; L=$T/build/debug/tools/launch/osoe-launch
for map in 0,0,0,0 0,0,1,1; do
  $L -n 4 -m $map -t 60 -- $O/sleeper >/dev/null 2>&1 & LP=$!
  sleep 4
  a=0; for p in $(pgrep -f "$O/sleeper"); do set -- $(cut -d' ' -f14,15 /proc/$p/stat); a=$((a + $1 + $2)); done
  sleep 10
  b=0; for p in $(pgrep -f "$O/sleeper"); do set -- $(cut -d' ' -f14,15 /proc/$p/stat); b=$((b + $1 + $2)); done
  echo "map $map: $((b - a)) clock ticks of CPU in 10 s across 4 idle PEs ($(getconf CLK_TCK) ticks/s; 4 cores = $((4 * 10 * $(getconf CLK_TCK))) ticks); threads per PE: $(ls /proc/$(pgrep -n -f "$O/sleeper")/task | wc -l)"
  pkill -KILL -f "$O/sleeper"; kill -KILL $LP 2>/dev/null; wait $LP 2>/dev/null; rm -f /dev/shm/osoe.$LP.*
done
