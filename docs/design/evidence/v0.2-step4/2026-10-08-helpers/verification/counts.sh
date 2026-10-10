#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The suite's sizes for the README: the full build's tests in debug and in
# release (with and without the models), the internode-labelled ones, and the
# single-node build's, by ctest -N on configured trees. usage: counts.sh TAR OUT.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; OUT=$2; : > $OUT
T=/tmp/v02-helpers-counts; rm -rf "${T:?}"; mkdir -p $T; tar -C $T -xf $TAR; cd $T || exit 1
cmake --preset debug > /dev/null 2>&1
echo "debug all: $(ctest --test-dir build/debug -N | tail -1)" >> $OUT
echo "debug internode: $(ctest --test-dir build/debug -N -L internode | tail -1)" >> $OUT
cmake --preset release > /dev/null 2>&1
echo "release all: $(ctest --test-dir build/release -N | tail -1)" >> $OUT
echo "release models: $(ctest --test-dir build/release -N -L '^model$' | tail -1)" >> $OUT
echo "release without models: $(ctest --test-dir build/release -N -LE '^model$' | tail -1)" >> $OUT
cmake --preset tsan > /dev/null 2>&1
echo "tsan all: $(ctest --test-dir build/tsan -N | tail -1)" >> $OUT
cmake --preset asan > /dev/null 2>&1
echo "asan all: $(ctest --test-dir build/asan -N | tail -1)" >> $OUT
cmake --preset debug -B build/single -DOSOE_SINGLE_NODE=ON > /dev/null 2>&1
echo "single-node debug all: $(ctest --test-dir build/single -N | tail -1)" >> $OUT
cd /; rm -rf "${T:?}"
echo DONE >> $OUT
