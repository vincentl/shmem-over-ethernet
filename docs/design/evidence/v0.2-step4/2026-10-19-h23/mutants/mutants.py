#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""H23's mutants: each new check must fail with the code it checks broken."""
import subprocess, sys
ROOT = '/tmp/h23-wip'
MUTANTS = [
    ('m1-seed-without-step', 'shmem/src/mesh-schedule.c',
     'osoe_wire_hash(((uint64_t)step << 32) | node_count)', 'osoe_wire_hash(node_count)',
     r'^shmem\.mesh-schedule$'),
    ('m2-opposite-upper-calls', 'shmem/src/mesh-schedule.c',
     'delta == node_count / 2 && from < to', 'delta == node_count / 2 && from > to',
     r'^shmem\.mesh-schedule$'),
    ('m3-no-rule-check', 'shmem/src/wire.c',
     'if (!osoe_mesh_schedule_calls(peer, b->node, b->node_count))', 'if (0)',
     r'^shmem\.mesh\.refused-caller$'),
    ('m4-slot-unbounded', 'shmem/src/bootstrap.c',
     '(slot == OSOE_ENVIRONMENT_OK && b->mesh_slot_us > OSOE_MESH_SLOT_MOST_US)', '0',
     r'^shmem\.start-up\.mesh-slot-large\.invalid$'),
    ('m5-hold-any-node', 'shmem/src/bootstrap.c',
     'node >= b->node_count || ms < 1', 'ms < 1',
     r'^shmem\.start-up\.test-mesh-hold-node\.invalid$'),
    ('m6-no-hold', 'shmem/src/wire.c',
     '        hold(b->mesh_hold_ms);', '        (void)hold;',
     r'^shmem\.mesh\.hold$'),
    ('m7-no-slot-clock', 'shmem/src/wire.c',
     'uint64_t due = begun_ns + call->slot * slot_ns;', 'uint64_t due = begun_ns;',
     r'^shmem\.mesh\.slot$'),
    ('m8-skip-late', 'shmem/src/wire.c',
     '''            if (due > now) {''',
     '''            if (due + slot_ns < now && m.begun > 0) {
                m.begun++;
                m.made++;
                continue;
            }
            if (due > now) {''',
     r'^shmem\.mesh\.crowd$'),
    ('m9-deaf-while-calling', 'shmem/src/wire.c',
     '''            int fd = osoe_listener_serve(l, purposes, p, &peer, &purpose);''',
     '''            int busy = 0;
            for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++)
                busy |= m.flying_call[k] != UINT32_MAX;
            int fd = busy ? -1 : osoe_listener_serve(l, purposes, p, &peer, &purpose);''',
     r'^shmem\.mesh\.none$'),
]

def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)

only = sys.argv[1:]
for name, path, old, new, tests in MUTANTS:
    if only and name not in only:
        continue
    original = open(f'{ROOT}/{path}').read()
    assert original.count(old) == 1, (name, original.count(old))
    open(f'{ROOT}/{path}', 'w').write(original.replace(old, new))
    try:
        b = sh('cmake --build build/release -j4 2>&1 | tail -3')
        built = sh('cmake --build build/release -j4 > /dev/null 2>&1; echo $?').stdout.strip()
        if built != '0':
            print(f'{name}: BUILD FAILED {b.stdout}', flush=True)
            continue
        r = sh(f'ctest --test-dir build/release --timeout 120 -R "{tests}" 2>&1')
        lines = [l for l in r.stdout.splitlines() if 'Test #' in l or 'tests passed' in l]
        failed = 'tests passed' in r.stdout and ', 0 tests failed' not in r.stdout
        print(f'{name}: {"FAILS (caught)" if failed else "PASSES (NOT CAUGHT)"}: ' + ' | '.join(lines), flush=True)
    finally:
        open(f'{ROOT}/{path}', 'w').write(original)
r = sh('cmake --build build/release -j4 > /dev/null 2>&1; echo $?')
print('rebuilt unmutated:', r.stdout.strip())
