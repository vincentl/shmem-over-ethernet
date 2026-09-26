#!/bin/bash
# Slice 3's mutations on a copy (wt/mutslice3), each shown in its build; the
# threaded, context, get, lock and quiet tests three times each in release.
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || { echo "lock busy" >&2; exit 1; }
R=/Users/vincent/shmem-over-ethernet
T=$R/build/scratch/wt/slice3
M=$R/build/scratch/wt/mutslice3
O=${OUT:-$R/build/scratch/slice3/m0}
mkdir -p $O; : > $O/mutate.txt
cd $M || exit 1
cmake --preset release > /dev/null 2>&1
FILES="shmem/src/transport-am.c"
restore() { for f in $FILES; do cp $T/$f $f; done; }
mutate() {
  python3 - "$1" <<'PY'
import sys
which = sys.argv[1]
edits = {
 'noretag': ("                if (atomic_load_explicit(&block->generation, memory_order_seq_cst) == g) {",
             "                if (1) {   /* MUTATION */"),
 'naive_clear': ("            if (!acknowledged(c, slot))\n                atomic_fetch_or_explicit(&c->dirty_bits[w], bit",
                 "            if (0)   /* MUTATION */\n                atomic_fetch_or_explicit(&c->dirty_bits[w], bit"),
 'bit_first': ("        _Atomic uint64_t *high = &c->shared_highest_segment[slot];\n        uint64_t seen",
               "        atomic_fetch_or_explicit(&c->dirty_bits[slot / 64], UINT64_C(1) << (slot % 64), memory_order_seq_cst);   /* MUTATION */\n        _Atomic uint64_t *high = &c->shared_highest_segment[slot];\n        uint64_t seen"),
 'bit_first_moved': ('        perturb();\n        _Atomic uint64_t *high = &c->shared_highest_segment[slot];\n        uint64_t seen = atomic_load_explicit(high, memory_order_seq_cst);\n        perturb();\n        while (seq > seen && !atomic_compare_exchange_weak_explicit(\n                                 high, &seen, seq, memory_order_seq_cst, memory_order_seq_cst))\n            ;\n        perturb();\n        _Atomic uint64_t *word = &c->dirty_bits[slot / 64];\n        uint64_t bit = UINT64_C(1) << (slot % 64);\n        if (!(atomic_load_explicit(word, memory_order_seq_cst) & bit))\n            atomic_fetch_or_explicit(word, bit, memory_order_seq_cst);\n',
                     '        perturb();\n        _Atomic uint64_t *word = &c->dirty_bits[slot / 64];\n        uint64_t bit = UINT64_C(1) << (slot % 64);\n        if (!(atomic_load_explicit(word, memory_order_seq_cst) & bit))\n            atomic_fetch_or_explicit(word, bit, memory_order_seq_cst);   /* MUTATION: the bit first */\n        perturb();\n        _Atomic uint64_t *high = &c->shared_highest_segment[slot];\n        uint64_t seen = atomic_load_explicit(high, memory_order_seq_cst);\n        perturb();\n        while (seq > seen && !atomic_compare_exchange_weak_explicit(\n                                 high, &seen, seq, memory_order_seq_cst, memory_order_seq_cst))\n            ;\n'),
 'plain_max': ("        while (seq > seen && !atomic_compare_exchange_weak_explicit(\n                                 high, &seen, seq, memory_order_seq_cst, memory_order_seq_cst))\n            ;",
               "        if (seq > seen) atomic_store_explicit(high, seq, memory_order_seq_cst);   /* MUTATION */"),
 'copier_pair_zero': ("bytes_copied[(token & OSOE_LANDING_PAIR) ? 1 : 0]", "bytes_copied[0] /* MUTATION */"),
 'pe_quiet_stores_full': ("    if (!pes && !only_gets)\n        atomic_store_explicit(&block->full",
                          "    if (!only_gets)   /* MUTATION */\n        atomic_store_explicit(&block->full"),
 'issue_locks': ("    /* No lock on a shared context: issue is lock-free (§4.11 item 4). */\n    remote_put(context, pe, region, offset, src, len, flags);",
                 "    context_enter(context); remote_put(context, pe, region, offset, src, len, flags); context_leave(context);   /* MUTATION */"),
 'no_own_scan': ("            if ((token & OSOE_LANDING_NONBLOCKING) &&\n                ((token >> OSOE_LANDING_OWNER_SHIFT)",
                 "            if (0 && (token & OSOE_LANDING_NONBLOCKING) &&   /* MUTATION */\n                ((token >> OSOE_LANDING_OWNER_SHIFT)"),
 'no_piggyback_guard': ("    if (full > e) {", "    if (full >= e) {   /* MUTATION */"),
}
old, new = edits[which]
p = 'shmem/src/transport-am.c'
s = open(p).read()
assert s.count(old) == 1, which
open(p, 'w').write(s.replace(old, new))
PY
}
run() {
  echo "== $1 ($(cat $FILES | grep -c MUTATION) mutated lines)" >> $O/mutate.txt
  cmake --build build/release -j4 2>&1 | grep -E " error|error:" | head -3 >> $O/mutate.txt
  ctest --test-dir build/release -j4 -R "${TESTS:-^shmem\.(multiple|thread|context|get-nbi|lock|quiet)}" \
    --repeat until-fail:${REPEAT:-3} --timeout 200 2>&1 \
    | grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run|Exception" \
    | sort -u | head -8 >> $O/mutate.txt
}
restore; run none
for m in ${MUTATIONS:-noretag naive_clear bit_first plain_max copier_pair_zero pe_quiet_stores_full issue_locks no_own_scan no_piggyback_guard}; do
  restore; mutate $m 2>> $O/mutate.txt; run $m
done
restore
echo DONE >> $O/mutate.txt
