#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# shmem.floor's stream ratio under design 8.4's protocol, for the pin: the final
# tree four times idle, four beside a ctest -j4, four under 16 busy loops; its
# poll mutant four times idle and twice under the loops. Release builds in
# /tmp/ladder/{final2,poll-final2}. Takes the VM lock. usage: pin-protocol.sh OUT
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; mkdir -p $OUT; : > $OUT/pin.txt
run() {   # build, label
  local B=/tmp/ladder/$1/build/release
  r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 600 -- $B/shmem/tests/shmem_test_floor 2>&1 | grep -oE 'stream library/floor=[0-9.]+ \([0-9.-]+\)')
  echo "$2 build=$1 $r" | tee -a $OUT/pin.txt
}
for i in 1 2 3 4; do run final2 "idle run=$i"; done
for i in 1 2 3 4; do run poll-final2 "idle run=$i"; done
# Beside a ctest -j4: the final tree's own tier-1 and tier-2 tests, but the
# timing pins' and the floor's, in a process group of its own.
setsid bash -c 'cd /tmp/ladder/final2/build/release && exec ctest -j4 -E "floor|echo-uring|^model" --repeat until-pass:1 > /dev/null 2>&1' &
CT=$!
sleep 5
for i in 1 2 3 4; do run final2 "beside-ctest run=$i"; done
kill -TERM -- -$(ps -o pgid= -p $CT | tr -d ' ') 2>/dev/null; wait $CT 2>/dev/null
pkill -x ctest 2>/dev/null; sleep 2
# Under 16 busy loops.
for k in $(seq 16); do setsid sh -c 'while :; do :; done' & echo $! >> /tmp/ladder-busy.pids; done
sleep 2
for i in 1 2 3 4; do run final2 "busy run=$i"; done
for i in 1 2; do run poll-final2 "busy run=$i"; done
for p in $(cat /tmp/ladder-busy.pids); do kill $p 2>/dev/null; done; rm -f /tmp/ladder-busy.pids
echo DONE >> $OUT/pin.txt
