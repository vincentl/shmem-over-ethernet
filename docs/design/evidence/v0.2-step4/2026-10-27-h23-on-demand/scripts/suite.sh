#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# One preset's suite, or a part of it, in the osoe VM under the VM's lock, with
# the environment given: H23's on-demand runs (2026-10-27).
#   suite.sh NAME PRESET [ENV=VALUE...] -- CTEST_ARGS...
set -u
NAME=$1 PRESET=$2
shift 2
envs=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
shift
exec 9>/tmp/osoe-vm.lock
flock -w 10800 9 || { echo "no lock" > "/tmp/h23-$NAME.status"; exit 1; }
cd /tmp/h23 || exit 2
echo "== $(date -u +%T) $NAME: ${envs[*]} ctest $*" > "/tmp/h23-$NAME.summary"
env "${envs[@]}" ctest --test-dir "build/$PRESET" "$@" > "/tmp/h23-$NAME.txt" 2>&1
grep -E 'tests passed|\(Failed\)|\(Timeout\)|\*\*\*Exception|Not Run' "/tmp/h23-$NAME.txt" >> "/tmp/h23-$NAME.summary"
echo "== $(date -u +%T) done" >> "/tmp/h23-$NAME.summary"
echo done > "/tmp/h23-$NAME.status"
