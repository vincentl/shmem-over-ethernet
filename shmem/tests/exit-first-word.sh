#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# An exit without shmem_finalize waits for a first word another thread is
# still writing (fatal.h; test-exit-first-word.c says how):
#
#   exit-first-word.sh LAUNCHER PROGRAM
#
# runs PROGRAM on one PE with standard error a pipe that is read only once the
# PE has made its mark and a second more has passed, and requires in it the
# line the PE holds there, the line of neither end - its exit's and its
# agent's, which lose the word - and the PE's exit with status 1.
set -u
LAUNCHER=$1 PROGRAM=$2
dir=$(mktemp -d)
cleanup() {
    rm -rf "$dir"
}
trap cleanup EXIT
mark=$dir/holding
# The reader waits 30 s at most for the mark: a PE that never makes it has
# failed, and what it wrote is read all the same.
out=$("$LAUNCHER" -n 1 -t 30 -- "$PROGRAM" "$mark" 2>&1 >/dev/null | {
    i=0
    while [ ! -e "$mark" ] && [ "$i" -lt 300 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    sleep 1
    tr -d '\000'
})
printf '%s\n' "$out"
held=$(printf '%s\n' "$out" | grep -c '^THE FIRST WORD$')
exits=$(printf '%s\n' "$out" | grep -c 'exited without calling shmem_finalize')
endings=$(printf '%s\n' "$out" | grep -c 'ending: the job is aborting')
failed=$(printf '%s\n' "$out" | grep -c '^osoe-launch: rank 0 exited with status 1$')
echo "exit-first-word.sh: the held line $held, the exit's $exits, the agent's $endings," \
    "rank 0's status 1 $failed"
[ "$held" -eq 1 ] && [ "$exits" -eq 0 ] && [ "$endings" -eq 0 ] && [ "$failed" -eq 1 ] &&
    echo "THE FIRST WORD WAITED FOR"
