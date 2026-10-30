#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release point's Pi runbook (docs/pi-cluster.md), part A, on the tree to be tagged, with T6's
# floor pins and the bench at no helpers beside the default (round ten's S3): the tree copied
# and built fresh per §2, then §1's facts, §2's probe and tiers, §4's smoke,
# §5's floor, §6's benchmark and §7's endings. Everything on the Pi under
# ~/osoe (outputs in ~/osoe/build/release-run). Usage: pi-a.sh TREE (from the Mac).
set -u
TREE=$1
OUT=<repo>/build/scratch/pi-tag-0edd95a9/a
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a -e 'ssh -o HostKeyAlias=pi-login.local' --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
$PI 'cd osoe && rm -rf build/release build/debug build/release-run build/compare build/corpus && mkdir -p build/release-run' || exit 1
$PI 'cd osoe/build/release-run && { srun -N4 --ntasks-per-node=1 uname -r; srun -N4 --ntasks-per-node=1 getconf PAGESIZE;
  srun -N4 --ntasks-per-node=1 cat /proc/sys/kernel/io_uring_disabled;
  srun -N4 --ntasks-per-node=1 cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor;
  srun -N4 --ntasks-per-node=1 sh -c "echo \"\$(hostname): \$(getent ahostsv4 \$(hostname) | head -1)\""; sinfo -V; } > facts.txt 2>&1; echo "facts: $?"' >> $LOG
$PI 'cd osoe && (cmake --preset release && cmake --build build/release -j4) > build/release-run/build.txt 2>&1; echo "build: $?"; grep -c "warning:" build/release-run/build.txt | sed "s/^/warnings: /"' >> $LOG
$PI 'cd osoe && srun -N4 --ntasks-per-node=1 build/release/tools/probe/probe > build/release-run/probe.txt 2>&1; echo "probe: $? $(grep -c "^ok" build/release-run/probe.txt) ok"' >> $LOG
$PI 'cd osoe && ctest --test-dir build/release -L tier1 -E "^model\." -j4 --timeout 300 > build/release-run/tier1.txt 2>&1; echo "tier1: $? $(grep "tests passed" build/release-run/tier1.txt)"' >> $LOG
$PI 'cd osoe && srun -N1 -n1 -c4 ctest --test-dir build/release -L tier2 -E "^model\." -j4 --timeout 300 > build/release-run/tier2.txt 2>&1; echo "tier2: $? $(grep "tests passed" build/release-run/tier2.txt)"' >> $LOG
$PI 'cd osoe && export OSOE_NO_PROGRESS_TIMEOUT=60; T="env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2 OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919";
  { srun -K1 -N4 --ntasks-per-node=4 $T build/release/shmem/tests/shmem_test_internode; echo "block exit $?";
    srun -K1 -N4 --ntasks-per-node=4 --distribution=cyclic $T build/release/shmem/tests/shmem_test_internode; echo "cyclic exit $?"; } > build/release-run/smoke.txt 2>&1; echo "smoke: $(grep exit build/release-run/smoke.txt | tr "\n" " ")"' >> $LOG
$PI 'cd osoe && srun -K1 -N2 --ntasks-per-node=1 -c4 build/release/shmem/tests/shmem_test_floor > build/release-run/floor.txt 2>&1; echo "floor: $?"' >> $LOG
# T6: the floor's pins as well (OSOE_TEST_FLOOR_PINS=1), each ratio held to its bound.
$PI 'cd osoe && OSOE_TEST_FLOOR_PINS=1 srun -K1 -N2 --ntasks-per-node=1 -c4 build/release/shmem/tests/shmem_test_floor > build/release-run/floor-pins.txt 2>&1; echo "floor pins: $?"' >> $LOG
$PI 'cd osoe && B=build/release/tools/bench/osoe-bench; R=build/release-run;
  srun -K1 -N2 --ntasks-per-node=1 -c4 $B > $R/bench-2x1.txt 2>&1; a=$?; srun -K1 -N4 --ntasks-per-node=1 -c4 $B > $R/bench-4x1.txt 2>&1; b=$?;
  srun -K1 -N4 --ntasks-per-node=4 $B > $R/bench-4x4.txt 2>&1; c=$?; srun -K1 -N4 --ntasks-per-node=4 --cpu-bind=none $B > $R/bench-4x4-unbound.txt 2>&1; d=$?;
  OSOE_PROGRESS_HELPERS=0 srun -K1 -N4 --ntasks-per-node=4 $B > $R/bench-4x4-helpers-0.txt 2>&1; e=$?;
  echo "bench: $a $b $c $d $e"' >> $LOG
$PI 'cd osoe && { srun -K0 -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_global-exit 7 1 compute; echo "global-exit status $?";
  srun -K0 -N4 --ntasks-per-node=4 build/release/shmem/tests/shmem_test_dead-pe 5; echo "dead-pe status $?"; } > build/release-run/endings.txt 2>&1; echo "endings: $(grep status build/release-run/endings.txt | tr "\n" " ")"' >> $LOG
scp -q 'pi-login:osoe/build/release-run/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
