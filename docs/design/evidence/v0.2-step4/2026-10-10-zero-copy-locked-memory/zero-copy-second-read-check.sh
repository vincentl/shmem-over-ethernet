#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The zero-copy refusal after its second adversarial read: the static checks;
# the refused tests and their neighbours in the debug preset, verbose; the two
# refused tests in AddressSanitizer and the wire's in ThreadSanitizer, whose
# runtimes made mlock() succeed and both tests skip there before the probe used
# the system call (finding 1); and the mutant that offers the refused bytes
# again only at a later pass, which the wire's test must now fail (finding 4).
# Usage: zero-copy-second-read-check.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
TAR=$1; D=$2
mkdir -p $D; OUT=$D/check.txt; : > $OUT
T=/tmp/v02-zero-copy-second-read; rm -rf "${T:?}"; mkdir -p $T; tar -C $T -xf $TAR; cd $T || exit 1
echo "kernel $(uname -r), ulimit -l $(ulimit -l)" >> $OUT
build() {   # preset
  echo "== build $1 $(date +%H:%M:%S)" >> $OUT
  cmake --preset $1 > $D/configure-$1.txt 2>&1 || { echo "configure failed $1" >> $OUT; return 1; }
  cmake --build build/$1 -j4 > $D/build-$1.txt 2>&1 || { echo "BUILD FAILED $1" >> $OUT; grep -E "error" $D/build-$1.txt | head -10 >> $OUT; return 1; }
  grep -E "warning:" $D/build-$1.txt | sort -u | head >> $OUT
}
run() {   # preset, label, regex
  ctest --test-dir build/$1 -V --timeout 300 -R "$3" > $D/$1-$2.full 2>&1
  grep -E "tests passed|Test +#.*(Passed|Failed|Timeout|Skipped|Not Run)|zero copy: |refused for want|SKIPPED|CHECK[(]" $D/$1-$2.full \
    | sed 's/^[0-9]*: //' >> $OUT
}
build release || exit 1
echo "== static $(date +%H:%M:%S)" >> $OUT
ctest --test-dir build/release -j4 --output-on-failure \
  -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' \
  > $D/static.full 2>&1
grep -E "tests passed|\(Failed\)" $D/static.full >> $OUT
build debug && { echo "== debug $(date +%H:%M:%S)" >> $OUT
  run debug zero-copy '^shmem\.(wire|internode\.zero-copy)'
  echo "== probe" >> $OUT; build/debug/tools/probe/probe 2>&1 | grep -E 'locked-memory' >> $OUT; }
build asan && { echo "== asan $(date +%H:%M:%S)" >> $OUT
  run asan refused '^shmem\.(wire|internode)\.zero-copy-refused'; rm -rf build/asan; }
build tsan && { echo "== tsan $(date +%H:%M:%S)" >> $OUT
  run tsan refused '^shmem\.(wire|internode)\.zero-copy-refused'; rm -rf build/tsan; }
echo "== mutant: the refused bytes offered again only at a later pass $(date +%H:%M:%S)" >> $OUT
python3 - <<'PY'
p = "shmem/src/ring.c"; s = open(p).read()
a = """                w->zero_copy = false;
                w->sending = false;
                w->sent = -1;"""
assert s.count(a) == 1
open(p, "w").write(s.replace(a, """                w->zero_copy = false;
                w->sent = 0;"""))
PY
cmake --build build/debug -j4 > $D/build-mutant.txt 2>&1 || { echo "mutant build failed" >> $OUT; exit 1; }
run debug mutant-later-pass '^shmem\.(wire|internode)\.zero-copy-refused'
echo "== done $(date +%H:%M:%S)" >> $OUT
