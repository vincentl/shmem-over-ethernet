#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The gate's check, its logic alone: a directory of report files written as
the tool writes them, beside a real canary's - round eight's two plants among
them (Q2-2: a Träff row read against a landing; two program threads on a lane
slot, no helper) - each of which the gate must fail but the last, a control.
usage: synthetic.py DIRECTORY CANARY_REPORT"""
import os, shutil, sys
d, canary = sys.argv[1], sys.argv[2]
shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
shutil.copy(canary, os.path.join(d, os.path.basename(canary)))

def frames(*names):
    return ''.join(f'    #{i} {n} /tmp/x/src/shmem/src/file.c:{10 + i} (libosoe.so.0+0x{i:x})\n'
                   for i, n in enumerate(names))

def race(pid, first, second, address, location_heap=True, helper=('T2', 'osoe-helper-1')):
    location = (f"osoe.1.0.1.0.heap.0 (deleted)+0x{address:x}" if location_heap else
                "test-circulant-reduce+0x1234")
    tid, tname = helper
    return (f'==================\nWARNING: ThreadSanitizer: data race (pid={pid})\n'
            f'  Write of size 8 at 0x{address:012x} by thread {first[0]}:\n{frames(*first[1])}\n'
            f'  Previous read of size 8 at 0x{address:012x} by {second[0]}:\n{frames(*second[1])}\n'
            f"  Location is global '<null>' at 0x000000000000 ({location})\n\n"
            f"  Thread {tid} '{tname}' (tid=4242, running) created by thread T1 at:\n"
            f'{frames("pthread_create", "start_helpers")}\n'
            f'SUMMARY: ThreadSanitizer: data race in memcpy\n==================\n')

landing = ('T2', ['memcpy', 'memcpy', 'osoe_am_parse', 'receive_wire', 'helper_main'])
# 12m heap: the internal region from 0xc00000; past the team words, a Traff row.
row = 0x010000000000 + 0xc00000 + 0x330100
files = {
    'shmem.circulant.4.900001': race(900001, landing, ('main thread', ['memcpy', 'round_take',
                                     'osoe_internode_generic_reduce']), row),
    'shmem.circulant.4.900002': race(900002, ('T5', ['memcpy', 'to_children']),
                                     ('thread T6', ['memcpy', 'to_children']), row,
                                     helper=('T5', 'worker')),
    'shmem.helpers.deaf.0-1.900003': '==================\nWARNING: ThreadSanitizer: thread leak (pid=900003)\n'
        "  Thread T2 'osoe-helper-1' (tid=4243, finished) created by thread T1 at:\n"
        + frames('pthread_create', 'start_helpers') +
        '\nSUMMARY: ThreadSanitizer: thread leak in start_helpers\n==================\n',
    'shmem.ack-echo.0-1.900004': 'ThreadSanitizer:DEADLYSIGNAL\n==900004==ERROR: ThreadSanitizer: SEGV on unknown address 0x000000000000\n',
    'shmem.circulant.4.900005': race(900005, landing, ('main thread', ['check_byte_sum', 'main']),
                                     0x010000000000 + 0x2000),
}
for name, text in files.items():
    open(os.path.join(d, name), 'w').write(text)
print(f'{len(files)} synthetic report files in {d}: the gate must fail the first four '
      'and pass the last')
