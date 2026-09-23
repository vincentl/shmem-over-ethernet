#!/bin/sh
# global-exit.sh LAUNCHER PROGRAM RANKS MAP HOW STATUS [CALLER [MODE]]
#
# Runs test-global-exit.c's job - PE CALLER calls shmem_global_exit(HOW), or
# with HOW=fatal ends the job with a fatal, while every other PE computes or
# streams - and requires every rank to have left with STATUS, none to have
# been killed or to have outlasted its computing, and, for a global exit, every
# rank's unflushed line to have been written by exit's flush (§9.1.6). MAP is
# the node map, or - for one node. CALLER (0) and MODE (compute) are
# test-global-exit.c's: which PE ends the job, and when and beside what. -k
# leaves the survivors to the library.
# WRAP, if set, is a command the launcher runs under: tier 3 runs it through
# tools/launch/tier3.sh, so that each node has a network namespace and a shaped
# link of its own (shmem/tests/CMakeLists.txt).
LAUNCHER=$1 PROGRAM=$2 RANKS=$3 MAP=$4 HOW=$5 STATUS=$6 CALLER=${7:-0} MODE=${8:-compute}
if [ "$MAP" = - ]; then
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -t 60 -k -- "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
else
    out=$(${WRAP:-} "$LAUNCHER" -n "$RANKS" -m "$MAP" -t 60 -k -- \
          "$PROGRAM" "$HOW" "$CALLER" "$MODE" 2>&1)
fi
echo "$out"
exited=$(printf '%s\n' "$out" | grep -c "exited with status $STATUS\$")
written=$(printf '%s\n' "$out" | grep -c "^PE [0-9]*: computing\$")
bad=$(printf '%s\n' "$out" | grep -c "NOT TERMINATED\|killed by signal\|timeout after")
echo "global exit: $exited of $RANKS ranks left with status $STATUS; $written of $RANKS" \
     "lines written; $bad ranks killed or still computing"
[ "$exited" -eq "$RANKS" ] || exit 1
[ "$bad" -eq 0 ] || exit 1
# Every line written, but for a fatal, which flushes nothing, and for an exit
# straight after init, which other PEs may meet before their own init returns.
[ "$HOW" = fatal ] || [ "$MODE" = immediate ] || [ "$written" -eq "$RANKS" ] || exit 1
echo "GLOBAL EXIT OK"
