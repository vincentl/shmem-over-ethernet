#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The gate's check, under the VM's lock: (1) its logic on synthetic reports;
# (2) plants in a tree built with ThreadSanitizer, each run with its control;
# (3) the helpers' release reverted - main's progress-host.c - against the four
# tests whose helpers leaked, three runs each way.
set -u
S=<repo>/build/scratch/review-nine-fixes; G=$S/gate-check; L=$G/logs; mkdir -p $L
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "lock taken $(date +%T)"
build() {                       # build TREE, a ThreadSanitizer build of the fixes' tree
  rm -rf $1; mkdir -p $1/src; tar -xf $S/tree.tar -C $1/src 2>/dev/null
  shift_ok=1
}
# (1) synthetic
B=/tmp/r9f/src/build/tsan
python3 -I $G/synthetic.py /tmp/r9f-synthetic $(ls -d $S/logs/full-tsan/reports-h2/tsan.reports.setup.* | head -1)
python3 -I /tmp/r9f/src/cmake/thread-sanitizer-gate.py judge /tmp/r9f-synthetic --ctest ctest --build $B \
  --place $B/osoe_thread_sanitizer_place > $L/synthetic.txt 2>&1; echo "synthetic: gate exit $?"; cat $L/synthetic.txt
mkdir -p /tmp/r9f-nocanary; rm -f /tmp/r9f-nocanary/*
python3 -I /tmp/r9f/src/cmake/thread-sanitizer-gate.py judge /tmp/r9f-nocanary --ctest ctest --build $B \
  --place $B/osoe_thread_sanitizer_place; echo "no canary: gate exit $?"
# (2) plants
P=/tmp/r9f-plant; rm -rf $P; mkdir -p $P/src; tar -xf $S/tree.tar -C $P/src 2>/dev/null
python3 -I $G/plant.py $P/src
cd $P/src
cmake --preset tsan > $L/plant-configure.txt 2>&1 && cmake --build --preset tsan -j4 > $L/plant-build.txt 2>&1 || { echo "plant build failed"; tail $L/plant-build.txt; exit 1; }
run() {                         # run LABEL HELPERS TEST [VAR=VALUE]
  local label=$1 h=$2 t=$3; shift 3
  env "$@" OSOE_PROGRESS_HELPERS=$h ctest --test-dir build/tsan --output-on-failure -R "^${t//./[.]}\$" > $L/$label.txt 2>&1
  printf '%-34s ctest %s; %s; gate: %s\n' "$label" "$?" \
    "$(grep -E "Test +#[0-9]+: ${t//./[.]} " $L/$label.txt | grep -oE 'Passed|\*\*\*Failed|Failed|\*\*\*Timeout|Exception' | head -1)" \
    "$(grep -oE 'THREAD SANITIZER GATE (PASSED|FAILED.*)' $L/$label.txt | head -1)"
}
run ack-echo-control-h2 2 shmem.ack-echo.0-1
run ack-echo-segv-h2 2 shmem.ack-echo.0-1 GATE_PLANT_SEGV=after
run ack-echo-segv-h0 0 shmem.ack-echo.0-1 GATE_PLANT_SEGV=after
run circulant-control-h0 0 shmem.circulant.4
run circulant-control-h2 2 shmem.circulant.4
run circulant-wrong-h2 2 shmem.circulant.4 GATE_PLANT_WRONG_PE=0
run circulant-race-global-h0 0 shmem.circulant.4 GATE_PLANT_RACE=global
run circulant-race-heap-h0 0 shmem.circulant.4 GATE_PLANT_RACE=heap
run circulant-race-heap-h2 2 shmem.circulant.4 GATE_PLANT_RACE=heap
for f in ack-echo-segv-h2 circulant-race-global-h0 circulant-race-heap-h0; do echo "-- $f"; grep -E '^shmem[.]|tsan.reports.setup' $L/$f.txt | grep -v 'Test ' | head -4; done
# (3) the release reverted
M=/tmp/r9f-leak; rm -rf $M; mkdir -p $M/src; tar -xf $S/tree.tar -C $M/src 2>/dev/null
cp $G/progress-host-main.c $M/src/shmem/src/progress-host.c; cp $G/am-state-main.h $M/src/shmem/src/am-state.h
cd $M/src
cmake --preset tsan > $L/leak-configure.txt 2>&1 && cmake --build --preset tsan -j4 > $L/leak-build.txt 2>&1 || { echo "leak build failed"; tail $L/leak-build.txt; exit 1; }
for tree in $M $P; do
  cd $tree/src
  for i in 1 2 3; do
    OSOE_PROGRESS_HELPERS=2 ctest --test-dir build/tsan -j4 --output-on-failure \
      -R 'shmem[.]helpers[.]deaf[.]0-1$|shmem[.]kill-after-init[.]TERM$|shmem[.]dead-pe[.]lead[.]0-0-1-1$|shmem[.]reply-pool[.]overflow$' \
      > $L/leaks-$(basename $tree)-$i.txt 2>&1
    echo "$(basename $tree) run $i: ctest $?; $(grep -oE '[0-9]+% tests passed.*' $L/leaks-$(basename $tree)-$i.txt); leaks $(grep -c 'thread leak' $L/leaks-$(basename $tree)-$i.txt); $(grep -oE 'THREAD SANITIZER GATE (PASSED|FAILED.*)' $L/leaks-$(basename $tree)-$i.txt | head -1)"
  done
done
echo "DONE $(date +%T)"
