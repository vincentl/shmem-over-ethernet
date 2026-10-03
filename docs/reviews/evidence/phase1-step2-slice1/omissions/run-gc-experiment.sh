#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Does `osoe-stat --gc` remove everything a killed multi-node job leaves?
T=${1:?}; O=${2:?}; L=$T/build/debug/tools/launch/osoe-launch; S=$T/build/debug/tools/stat/osoe-stat
OSOE_WATCHDOG=0 $L -n 4 -m 0,0,1,1 -k -t 60 -- $O/sleeper >/dev/null 2>&1 & LP=$!
sleep 3; kill -KILL $LP; pkill -KILL -f "$O/sleeper"; sleep 1
echo "corpse of job $LP:"; ls -la /dev/shm | grep "osoe\.$LP\." | awk '{print "  " $5, $9}'
echo "osoe-stat --gc:"; $S --gc 2>&1 | grep "\.$LP\." | sed 's/^/  /'
echo "after --gc:"; ls -la /dev/shm | grep "osoe\.$LP\." | awk '{print "  " $5, $9}'
rm -f /dev/shm/osoe.$LP.*
