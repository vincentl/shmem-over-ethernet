#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# mutants.sh DIR N OUT: the fix's two parts, each taken away in turn from the
# fixed tree DIR (built in debug by setup.sh), under the VM's lock; for each,
# shmem.fatal-order and N runs of shmem.death-in-wait.barrier.L4 (repeat.sh's
# loop, its outputs in OUT/<mutant>); the tree restored and rebuilt after.
#   no-await: osoe_fatal_await_first_word returns at once - every end goes on
#             under a first word still being written, as before the fix; the
#             word still taken right before the write.
#   wide:     the word taken, then stdio flushed and a stat made before the
#             write, as fatal() held it before the fix; the awaits kept.
exec 9>/tmp/osoe-vm.lock; flock -n 9 || { echo "VM LOCK HELD"; exit 1; }
D=$1 N=$2 OUT=$3
cd "$D" || exit 1
B=build/debug
cp shmem/src/fatal.c "$OUT.fatal.c.orig" || exit 1
cmd=$(ctest --test-dir $B -N -V -R '^shmem\.death-in-wait\.barrier\.L4$' | sed -n 's/^[0-9]*: Test command: //p')
wd=$(ctest --test-dir $B -N -V -R '^shmem\.death-in-wait\.barrier\.L4$' | sed -n 's/^[0-9]*: Working Directory: //p')
runs() {
    local out=$OUT/$1 pass=0 fail=0
    mkdir -p "$out"; rm -f "$out"/run-*.txt "$out/tally.txt"
    for i in $(seq "$N"); do
        (cd "$wd" && eval "$cmd") > "$out/run-$i.txt" 2>&1
        s=$?
        agent=$(grep -c ': ending: the job is aborting' "$out/run-$i.txt")
        sweep=$(grep -c ': shmem_barrier_all: the job is aborting' "$out/run-$i.txt")
        echo "$i status $s agent $agent sweep $sweep" >> "$out/tally.txt"
        if [ $s = 0 ] && grep -q 'DEATH IN WAIT OK' "$out/run-$i.txt"; then
            pass=$((pass + 1)); rm -f "$out/run-$i.txt"
        else
            fail=$((fail + 1))
        fi
    done
    echo "$1: death-in-wait.barrier.L4 passed $pass, failed $fail of $N; lines agent" \
         "$(awk '{a+=$5} END {print a}' "$out/tally.txt") sweep $(awk '{a+=$7} END {print a}' "$out/tally.txt")"
}
mutate() {
    python3 -I - "$1" <<'PY'
import sys
p = 'shmem/src/fatal.c'
s = open(p).read()
if sys.argv[1] == 'no-await':
    a = ('    while (atomic_load_explicit(&first_word, memory_order_acquire) == BEING_SAID &&\n'
         '           !osoe_deadline_expired(&until))\n')
    b = ('    while (0 && atomic_load_explicit(&first_word, memory_order_acquire) == BEING_SAID &&\n'
         '           !osoe_deadline_expired(&until))\n')
else:
    a = ('        return false;\n    ssize_t ignored = write(STDERR_FILENO, line, len);\n')
    b = ('        return false;\n    fflush(NULL);\n    struct stat st;\n'
         '    (void)stat("/dev/shm/osoe-wide-mutant", &st);\n'
         '    ssize_t ignored = write(STDERR_FILENO, line, len);\n')
assert s.count(a) == 1, sys.argv[1]
open(p, 'w').write(s.replace(a, b))
PY
}
for m in no-await wide; do
    echo "== mutant $m"
    cp "$OUT.fatal.c.orig" shmem/src/fatal.c
    mutate $m || { echo "MUTATION FAILED $m"; exit 1; }
    if ! cmake --build $B -j4 > "$OUT.build-$m.txt" 2>&1; then
        echo "BUILD FAILED $m"; grep -E "error" "$OUT.build-$m.txt" | head; exit 1
    fi
    ctest --test-dir $B --timeout 120 -R '^shmem\.fatal-order$' --output-on-failure > "$OUT.fatal-order-$m.txt" 2>&1
    echo "$m: shmem.fatal-order $(grep -E 'tests passed' "$OUT.fatal-order-$m.txt")"
    grep -E 'CHECK\(|left:' -A1 "$OUT.fatal-order-$m.txt" | head -4
    runs $m
done
cp "$OUT.fatal.c.orig" shmem/src/fatal.c
cmake --build $B -j4 > "$OUT.rebuild.txt" 2>&1; echo "restored and rebuilt $?"
ctest --test-dir $B --timeout 120 -R '^shmem\.fatal-order$' 2>&1 | grep -E 'tests passed'
echo "MUTANTS DONE $(date +%H:%M:%S)"
