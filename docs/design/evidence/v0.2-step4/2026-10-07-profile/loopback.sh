#!/bin/bash
# v0.2 step 4 part 0: the loopback numbers for the spare, the cap and deeper
# buffers, and the floor pin's mutant. Builds are in /tmp/ladder. Takes the lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; mkdir -p $OUT; : > $OUT/loopback.txt
S=/Users/vincent/shmem-over-ethernet/build/scratch/ladder
run() {   # build, label, environment, bytes, seconds
  local B=/tmp/ladder/$1/build/release W=/tmp/ladder/$1/work
  mkdir -p $W; [ -x $W/putstream ] || $B/tools/wrapper/oshcc -O2 -o $W/putstream $S/putstream.c
  r=$(env $3 $B/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- $W/putstream $4 $5 2>&1 | grep '^putstream')
  echo "$2 $r" | tee -a $OUT/loopback.txt
}
echo "== the stream: base, fill, spare" >> $OUT/loopback.txt
for i in 1 2 3; do for b in base fill2 spare; do run $b "round=$i build=$b" "" 65536 10; done; done
echo "== the cap on loopback: the spare with the kernel's unsent queue capped" >> $OUT/loopback.txt
for i in 1 2 3; do for l in 0 16384 131072; do run spare-lowat "round=$i lowat=$l" "OSOE_EXPERIMENT_NOTSENT_LOWAT=$l" 65536 10; done; done
echo "== deeper buffers on top of the spare" >> $OUT/loopback.txt
for i in 1 2; do for buf in 131072 262144 524288 1048576; do run spare "round=$i buffer=$buf" "OSOE_AM_BUFFER=$buf" 65536 10; done; done
echo "== the floor test's stream ratio: the spare, and the polling mutant of base and spare" >> $OUT/loopback.txt
for i in 1 2 3; do for b in spare poll-base poll-spare; do
  B=/tmp/ladder/$b/build/release
  r=$($B/tools/launch/osoe-launch -n 2 -m 0,1 -t 300 -- $B/shmem/tests/shmem_test_floor 2>&1 | grep -oE 'stream library/floor=[0-9.]+ \([0-9.-]+\)')
  echo "round=$i build=$b $r" | tee -a $OUT/loopback.txt
done; done
echo DONE >> $OUT/loopback.txt
