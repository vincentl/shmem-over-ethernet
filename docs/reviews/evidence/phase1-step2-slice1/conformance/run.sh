#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Build and run the conformance probes against a PRIVATE build of the tree
# (never build/debug). Run inside the Lima VM:
#   limactl shell osoe sh docs/reviews/evidence/phase1-step2-slice1/conformance/run.sh
ROOT=<repo>
TREE=$ROOT/build/scratch/review-conformance/tree
B=$TREE/build/debug
HERE=$ROOT/docs/reviews/evidence/phase1-step2-slice1/conformance
OUT=$ROOT/build/scratch/review-conformance/bin
LAUNCH=$B/tools/launch/osoe-launch
mkdir -p $OUT
export LD_LIBRARY_PATH=$B/shmem
build() { # name
    cc -std=c11 -Wall -Wextra -I$TREE/shmem/include -o $OUT/$1 $HERE/$1.c -L$B/shmem -losoe -lm 2>&1
}
echo "##### compile-only probes"
for t in pcontrol-from-shmem-header mpp-shmemx-header deprecated-amo-names spec-example-5; do
    echo "--- $t"; build $t | grep -E "error|warning: implicit|undefined reference" | head -6
    echo "exit: $( [ -x $OUT/$t ] && echo built || echo NOT-BUILT )"
done
echo "##### spec examples, 4 PEs on 2 logical nodes (-m 0,0,1,1) and on one node"
for t in spec-example-38 spec-example-42 spec-example-43; do
    build $t
    for m in 0,0,0,0 0,0,1,1 0,1,2,3; do
        $LAUNCH -n 4 -m $m -t 30 -- $OUT/$t >/tmp/conf.out 2>&1; echo "--- $t map $m: exit $?"; head -3 /tmp/conf.out
    done
done
echo "##### SMA_SYMMETRIC_SIZE"
build heap-size-probe
echo "--- default:";               $LAUNCH -n 2 -t 30 -- $OUT/heap-size-probe 2>&1 | head -3
echo "--- SHMEM_SYMMETRIC_SIZE=1m:"; SHMEM_SYMMETRIC_SIZE=1m $LAUNCH -n 2 -t 30 -- $OUT/heap-size-probe 2>&1 | head -3
echo "--- SMA_SYMMETRIC_SIZE=1m:";   SMA_SYMMETRIC_SIZE=1m $LAUNCH -n 2 -t 30 -- $OUT/heap-size-probe 2>&1 | head -3
echo "--- SMA_SYMMETRIC_SIZE=garbage:"; SMA_SYMMETRIC_SIZE=garbage $LAUNCH -n 2 -t 30 -- $OUT/heap-size-probe 2>&1 | head -3
echo "##### SHMEM_SYMMETRIC_SIZE syntax (heap bytes from SHMEM_DEBUG)"
for v in 20m 3.1M .5m 0.5m 20kk 1.5 5. 0 1e6 "+5m" " 5m" 5mb 1K; do
    printf -- "--- '%s': " "$v"
    SHMEM_DEBUG=1 SHMEM_SYMMETRIC_SIZE="$v" $LAUNCH -n 1 -t 30 -- $OUT/heap-size-probe 2>&1 | grep -oE "heap [0-9]+ bytes|is invalid.*|osoe:.*" | head -1
done
echo "##### test_any / wait_until_any fairness"
build test-any-fairness
$LAUNCH -n 1 -t 60 -- $OUT/test-any-fairness 2>&1 | head -4
echo "##### SHMEM_TEAM_SHARED vs shmem_ptr"
build team-shared-contract
for m in 0,0,0,0 0,0,1,1 0,1,2,0; do
    echo "--- map $m"; $LAUNCH -n 4 -m $m -t 30 -- $OUT/team-shared-contract 2>&1 | head -6
done
echo "--- map 0,0,1,1 with OSOE_FORCE_REMOTE=1"
OSOE_FORCE_REMOTE=1 $LAUNCH -n 4 -m 0,0,1,1 -t 30 -- $OUT/team-shared-contract 2>&1 | head -8
echo "##### lifecycle"
build lifecycle
for m in 0,0 0,1; do echo "--- map $m"; $LAUNCH -n 2 -m $m -t 30 -- $OUT/lifecycle 2>&1 | head -8; echo "exit $?"; done
echo "##### calls into an uninitialized library (1 PE)"
build uninitialized-calls
for w in query info my_pe pe_accessible ptr finalize team_my_pe malloc free putmem atomic wait quiet barrier_all team_sync \
         +my_pe +ptr +malloc +putmem +quiet +barrier_all +team_my_pe; do
    timeout 20 $LAUNCH -n 1 -t 10 -- $OUT/uninitialized-calls $w >/tmp/conf.out 2>&1; rc=$?
    printf -- "--- %-14s exit %-3s %s\n" "$w" "$rc" "$(grep -v '^returned' /tmp/conf.out | head -2 | tr '\n' '|' | cut -c1-200)"
done
echo "##### a fatal error under shmem_init and under start_pes (2 PEs, watchdog on and off)"
build fatal-under-start-pes
for mode in init start_pes; do for wd in 1 0; do
    s=$(date +%s); OSOE_WATCHDOG=$wd $LAUNCH -n 2 -t 15 -- $OUT/fatal-under-start-pes $mode >/tmp/conf.out 2>&1; rc=$?
    echo "--- $mode OSOE_WATCHDOG=$wd: exit $rc after $(( $(date +%s) - s )) s"; head -4 /tmp/conf.out | cut -c1-220
done; done
echo "##### completion seen from a third PE; multi-chunk put-with-signal"
build completion-probe
for m in 0,0,0 0,1,2 0,1,1,0 0,1,0,1; do
    n=$(echo $m | tr ',' '\n' | wc -l)
    OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919 \
        $LAUNCH -n $n -m $m -t 120 -- $OUT/completion-probe >/tmp/conf.out 2>&1; echo "--- map $m: exit $?"; head -4 /tmp/conf.out
done
echo "##### header properties under C99, C11, C++"
for std in c99 c11 c17; do
    cc -std=$std -pedantic-errors -Wall -Wextra -Werror -I$TREE/shmem/include -o $OUT/header-$std $HERE/header-properties.c -L$B/shmem -losoe 2>&1 | head -5
    echo "--- -std=$std: $( $OUT/header-$std && echo ok )"
done
c++ -x c++ -std=c++17 -Wall -Wextra -I$TREE/shmem/include -o $OUT/header-cxx $HERE/header-properties.c -L$B/shmem -losoe 2>&1 | head -5
echo "--- c++17: $( [ -x $OUT/header-cxx ] && $OUT/header-cxx && echo ok )"
echo "##### signed and exact-width AMOs"
build amo-signed-values
for m in 0,0 0,1; do $LAUNCH -n 2 -m $m -t 30 -- $OUT/amo-signed-values 2>&1 | grep -v "wire split" | head -3; done
