#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The read's R9F-3 and R9F-4 in the gate: the settings of shmem.link.few-lanes.0-1
# (link.sh sets the fan-out), and a file whose one report passes beside a FATAL
# line and beside a CHECK failed line - each must fail.
set -u
S=<repo>/build/scratch/review-nine-fixes; G=$S/gate-check
exec 9>/tmp/osoe-vm.lock; flock -w 43200 9 || exit 1
cd /tmp/r9f/src && tar -xf $S/tree.tar cmake/thread-sanitizer-gate.py shmem/tests/link.sh 2>/dev/null
B=build/tsan
python3 -I - <<'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location('gate', 'cmake/thread-sanitizer-gate.py')
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
s = g.test_settings('ctest', 'build/tsan')
print('few-lanes settings:', s.get('shmem.link.few-lanes.0-1'))
print('circulant.5.ceiling-2 settings:', s.get('shmem.circulant.5.ceiling-2'))
PY
D=/tmp/r9f-fatal; rm -rf $D; mkdir -p $D
cp $(ls -d $S/logs/full-tsan/reports-h2/tsan.reports.setup.* | head -1) $D/
good=$(grep -l 'osoe-helper' $S/logs/full-tsan/reports-h2/shmem.circulant.4.* 2>/dev/null | head -1)
[ -n "$good" ] || good=$(grep -L 'thread leak' $S/logs/full-tsan/reports-h2/shmem.* | head -1)
name=$(basename $good)
{ cat $good; echo "FATAL: ThreadSanitizer: unexpected memory mapping 0x7f0000000000-0x7f0000001000"; } > $D/$name
{ cat $good; echo "ThreadSanitizer: CHECK failed: tsan_rtl.cpp:123 \"((x)) != (0)\" (0x0, 0x0) (tid=1)"; } > $D/${name%.*}.900099
python3 -I cmake/thread-sanitizer-gate.py judge $D --ctest ctest --build $B --place $B/osoe_thread_sanitizer_place; echo "gate exit $? (must be 1)"
