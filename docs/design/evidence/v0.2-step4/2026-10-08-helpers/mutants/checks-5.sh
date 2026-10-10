#!/bin/bash
# v0.2 step 4 part 3, the second adversarial read's answers: the fixed tree's
# changed areas in debug - the static checks, the global-exit harness's tests
# and the helpers', the pacer's mix tests - with the new reset-stream run ten
# times; then the mutants each answer must be caught by: the flush that sends on
# a failed wire and the node excuse removed (reset-stream, ten runs each), the
# pacer's old reading with PE 1 an hour late (mix-end, three runs); then in tsan:
# helper-wake left out, reset-stream there, and stopped-bulk at two helpers
# given ten minutes, to tell TSan's slowness from a hang.
# usage: checks-5.sh DIR (fixed.tar, unfixed-flush.tar, no-excuse.tar,
# unagreed.tar). Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 21600 9 || exit 1
D=$1; OUT=$D/checks.txt; : > "$OUT"
build() {   # name preset
  T=/tmp/helpers-checks-$1-$2; rm -rf "${T:?}"; mkdir -p "$T"; tar -C "$T" -xf "$D/$1.tar"
  echo "== build $1 $2 $(date +%H:%M:%S)" >> "$OUT"
  if ! (cd "$T" && cmake --preset "$2" > "$D/$1-$2.configure" 2>&1 &&
        cmake --build "build/$2" -j4 > "$D/$1-$2.build" 2>&1); then
    echo "BUILD FAILED $1 $2" >> "$OUT"; grep -E "error" "$D/$1-$2.build" | head -5 >> "$OUT"
    return 1
  fi
  grep -E "warning:" "$D/$1-$2.build" | sort -u | head >> "$OUT"
}
run() {     # name preset label ctest-arguments...
  name=$1 preset=$2 label=$3; shift 3
  echo "== $name $preset $label $(date +%H:%M:%S)" >> "$OUT"
  ctest --test-dir "/tmp/helpers-checks-$name-$preset/build/$preset" --timeout 300 \
        --output-on-failure "$@" > "$D/$name-$preset-$label.full" 2>&1
  grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|No tests were found" \
       "$D/$name-$preset-$label.full" | head -12 >> "$OUT"
}
count() {   # name preset times regex
  name=$1 preset=$2 times=$3 regex=$4; failed=0
  for i in $(seq 1 "$times"); do
    ctest --test-dir "/tmp/helpers-checks-$name-$preset/build/$preset" --timeout 300 \
          --output-on-failure -R "$regex" > "$D/$name-$preset-count-$i.full" 2>&1 ||
      failed=$((failed + 1))
  done
  echo "== $name $preset: $regex failed $failed of $times $(date +%H:%M:%S)" >> "$OUT"
  grep -h -m2 -E "connection to node [0-9]+ failed|without its goodbye|reset-stream: |osoe: deadlock: every" \
       "$D/$name-$preset"-count-*.full | sort | uniq -c | head -6 >> "$OUT"
}
if build fixed debug; then
  run fixed debug changed -j4 -R '^shmem\.(exports|boundary|memory-orders|static-link|strict-c11)$|global-exit|^shmem\.helpers\.|^tools\.pacer\.mix'
  count fixed debug 10 '^shmem\.helpers\.reset-stream'
fi
for m in unfixed-flush no-excuse; do
  build $m debug && count $m debug 10 '^shmem\.helpers\.reset-stream'
  rm -rf "/tmp/helpers-checks-$m-debug"
done
build unagreed debug && count unagreed debug 3 '^tools\.pacer\.mix-end'
rm -rf /tmp/helpers-checks-unagreed-debug
count fixed debug 3 '^tools\.pacer\.mix-end'
rm -rf /tmp/helpers-checks-fixed-debug
if build fixed tsan; then
  echo "helper-wake under tsan: $(ctest --test-dir /tmp/helpers-checks-fixed-tsan/build/tsan -N -R helper-wake | tail -1)" >> "$OUT"
  echo "tests under tsan: $(ctest --test-dir /tmp/helpers-checks-fixed-tsan/build/tsan -N | tail -1)" >> "$OUT"
  count fixed tsan 3 '^shmem\.helpers\.reset-stream'
  B=/tmp/helpers-checks-fixed-tsan/build/tsan
  echo "== fixed tsan: stopped-bulk at two helpers, ten minutes $(date +%H:%M:%S)" >> "$OUT"
  start=$(date +%s)
  env OSOE_PROGRESS_HELPERS=2 OSOE_TEST_STOP_MS=15000 OSOE_KEEPALIVE=1,1,3 SHMEM_SYMMETRIC_SIZE=128m \
      "$B/tools/launch/osoe-launch" -n 3 -m 0,0,1 -t 600 -- "$B/shmem/tests/shmem_test_stopped-bulk" \
      > "$D/stopped-bulk-tsan-h2.txt" 2>&1
  status=$?
  echo "launcher exit $status after $(( $(date +%s) - start )) s; $(grep -h 'STOPPED BULK OK\|PE 0: the put returned\|timeout after\|exited with status' "$D/stopped-bulk-tsan-h2.txt" | head -5 | tr '\n' ';')" >> "$OUT"
  rm -rf /tmp/helpers-checks-fixed-tsan
fi
echo "DONE $(date +%H:%M:%S)" >> "$OUT"
