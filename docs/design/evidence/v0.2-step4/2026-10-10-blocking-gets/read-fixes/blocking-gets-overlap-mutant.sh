#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The read's note 4: a blocking get that completes each piece before it issues
# the next must fail shmem.get-pieces.0-1 now (am.landing_full), where it passed
# the test as first written. Mutates the check's tree in place, after its run.
exec 9>/tmp/osoe-vm.lock; flock -w 600 9 || exit 1
cd /tmp/v02-blocking-gets-read || exit 1
python3 - <<'PY'
p = "shmem/src/transport-internode.c"; s = open(p).read()
a = """        issue_get_pieces(&c, pe, region, offset, dst, len, 0, "shmem_get", true);
        complete_gets(&c, "shmem_get", "the bytes of its get's pieces", peer, pe);"""
b = """        for (size_t done = 0; done < len; done += limit) {
            size_t n = len - done < limit ? len - done : limit;
            issue_get_pieces(&c, pe, region, offset + done, (unsigned char *)dst + done, n, 0,
                             "shmem_get", true);
            complete_gets(&c, "shmem_get", "the bytes of its get's pieces", peer, pe);
        }"""
assert s.count(a) == 1
open(p, "w").write(s.replace(a, b))
PY
cmake --build build/debug -j4 > /dev/null 2>&1 || { echo "mutant build failed"; exit 1; }
ctest --test-dir build/debug -V -R 'shmem\.get-pieces\.0-1$' 2>&1 | grep -E "a get of|CHECK|GET PIECES|Passed|Failed"
