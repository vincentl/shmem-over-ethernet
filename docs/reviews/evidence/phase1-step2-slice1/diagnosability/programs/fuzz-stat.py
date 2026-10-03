#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""fuzz-stat.py SEGMENT OSOE_STAT [ROUNDS]

Copy a live control segment, damage the copy, and run osoe-stat over it in each
output mode. A reader used on a machine where something has already gone wrong
must never die on a signal. Damage classes:
  directory  random 1-8 byte overwrites inside the 136-byte frozen directory
             (magic kept, so the reader gets past the first check)
  table      random overwrites inside the metric table, then the checksum is
             RECOMPUTED so the damaged table is accepted (a buggy or hostile
             writer, not a bit flip)
  truncate   the file is cut short of what the directory says
The copy is named osoe.fuzz<pid>.<n> and removed afterwards.
"""
import os, random, struct, subprocess, sys

seg, stat = sys.argv[1], sys.argv[2]
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 300
data = open(seg, 'rb').read()
random.seed(12345)

# The frozen directory, as osoe-stat --dump reports it: find the table through
# the tool's own report rather than the library's headers.
out = subprocess.run([stat, '--dump', seg], capture_output=True, text=True).stdout
line = [l for l in out.splitlines() if l.strip().startswith('metrics ')][0].split()
table_offset, table_count, descriptor = int(line[1]), int(line[3]), int(line[5])
table_bytes = table_count * descriptor

def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xffffffff
    return h

# Locate the checksum field by searching the directory for the known value.
good = fnv(data[table_offset:table_offset + table_bytes])
checksum_at = data[:136].find(struct.pack('<I', good))
assert checksum_at > 0, 'checksum not found in directory'

crashes = {}
for n in range(rounds):
    b = bytearray(data)
    kind = ('directory', 'table', 'truncate')[n % 3]
    if kind == 'directory':
        for _ in range(random.randint(1, 3)):
            at = random.randint(8, 135)
            w = random.choice((1, 2, 4, 8))
            v = random.choice((0, 1, 0xff, 0xffffffff, 0xffffffffffffffff, 0x7fffffff,
                               random.getrandbits(64), len(b), len(b) - 1, len(b) + 1))
            b[at:at + w] = struct.pack('<Q', v & 0xffffffffffffffff)[:w]
    elif kind == 'table':
        for _ in range(random.randint(1, 6)):
            d = random.randrange(table_count)
            at = table_offset + d * descriptor + random.randint(40, descriptor - 1)
            w = random.choice((1, 2, 4))
            v = random.choice((0, 0xff, 0xffff, 0xffffffff, random.getrandbits(32), 3, 7, 511, 513))
            b[at:at + w] = struct.pack('<I', v)[:w]
        b[checksum_at:checksum_at + 4] = struct.pack('<I', fnv(b[table_offset:table_offset + table_bytes]))
    else:
        b = b[:random.choice((136, 137, 1536, table_offset + 5, table_offset + table_bytes,
                              len(b) // 2, len(b) - 1, random.randint(136, len(b))))]
    name = '/dev/shm/osoe.fuzz%d.%d' % (os.getpid(), n)
    open(name, 'wb').write(b)
    try:
        for mode in (['--counters'], ['--dump'], ['--threads'], ['--dump', '--json'], ['--counters', '--json']):
            try:
                r = subprocess.run([stat] + mode + [name], capture_output=True, timeout=20)
                rc = r.returncode
            except subprocess.TimeoutExpired:
                rc = 'timeout'
            if rc == 'timeout' or rc < 0:
                crashes.setdefault((kind, rc, mode[0] + (mode[1] if len(mode) > 1 else '')), []).append(n)
                keep = '/dev/shm/osoe.fuzz-keep.%s.%d' % (kind, n)
                if len(crashes[(kind, rc, mode[0] + (mode[1] if len(mode) > 1 else ''))]) == 1:
                    open(keep, 'wb').write(b)
    finally:
        os.unlink(name)

print('rounds', rounds, 'table at', table_offset, 'count', table_count, 'checksum field at', checksum_at)
if not crashes:
    print('no signal and no timeout in any mode')
for k, v in sorted(crashes.items(), key=str):
    print('DIED', k, 'rounds', v[:8], '(%d total)' % len(v))
