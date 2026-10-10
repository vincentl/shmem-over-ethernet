#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Part 3's mutants: each a copy of the worktree with one change, packed as
NAME.tar beside NAME.regex (the tests that should fail on it) and NAME.env."""
import os, shutil, subprocess, sys, tempfile

TREE = sys.argv[1]
OUT = sys.argv[2]

MUTANTS = [
    ('wake-other-owner', 'shmem/src/progress-host.c',
     """static uint32_t owner_of_peer(uint32_t peer)
{
    uint32_t index;
    return osoe_connection_area_owner(&osoe_am.area, peer, &index);
}""",
     """static uint32_t owner_of_peer(uint32_t peer)
{
    uint32_t index;
    uint32_t owner = osoe_connection_area_owner(&osoe_am.area, peer, &index);
    return owner == 0 ? 0 : owner % osoe_am.area.helpers + 1;
}""",
     r'shmem\.helper-wake', ''),
    ('deaf-unwatched', 'shmem/src/progress-host.c',
     """        if (turns - posted_at >= 2) {""",
     """        if (false && turns - posted_at >= 2) {""",
     r'shmem\.helpers\.deaf', ''),
    ('helpers-counted', 'shmem/src/deadlock.c',
     """static bool progress_helper(const struct osoe_pe_slot *s, int32_t tid)
{
    if (s != &osoe.node.slots[0])""",
     """static bool progress_helper(const struct osoe_pe_slot *s, int32_t tid)
{
    return false;
    if (s != &osoe.node.slots[0])""",
     r'shmem\.deadlock\.(waits|reinit|mismatch)\.0-[01]-[01]-1', 'OSOE_PROGRESS_HELPERS=2'),
    ('unsent-agent-only', 'shmem/src/progress-host.c',
     """        lower_flag(helper);
        take_unsent_request(helper);""",
     """        lower_flag(helper);""",
     r'shmem\.unsent\.(set|by-hand|floor)', 'OSOE_PROGRESS_HELPERS=2'),
    ('no-abort-excuse', 'shmem/src/progress.c',
     """        (o->aborting || *peer_aborted || (other_aborted && *other_aborted) ||
         node_aborting(o->engine))) {""",
     """        (o->aborting || *peer_aborted || (other_aborted && *other_aborted))) {""",
     r'shmem\.(global-exit|kill-after-init|abort|fatal|stopped|launcher-signals)', 'OSOE_PROGRESS_HELPERS=2'),
    ('abort-without-helpers', 'shmem/src/progress-host.c',
     """    abort_wires(o, &grace);
    wait_helpers(&grace);
    leave_helpers();""",
     """    abort_wires(o, &grace);
    leave_helpers();""",
     r'shmem\.(global-exit|kill-after-init|abort|fatal|stopped|launcher-signals)', 'OSOE_PROGRESS_HELPERS=2'),
]

os.makedirs(OUT, exist_ok=True)
for name, path, old, new, regex, env in MUTANTS:
    work = tempfile.mkdtemp(prefix=f'mutant-{name}-')
    subprocess.run(['rsync', '-a', '--exclude', 'build', '--exclude', '.git', '--exclude', '._*',
                    TREE + '/', work + '/'], check=True)
    p = os.path.join(work, path)
    s = open(p).read()
    assert s.count(old) == 1, (name, s.count(old))
    open(p, 'w').write(s.replace(old, new))
    subprocess.run(['tar', '--no-mac-metadata', '-C', work, '-cf', os.path.join(OUT, name + '.tar'), '.'],
                   check=True, env=dict(os.environ, COPYFILE_DISABLE='1'))
    open(os.path.join(OUT, name + '.regex'), 'w').write(regex + '\n')
    if env:
        open(os.path.join(OUT, name + '.env'), 'w').write(env.replace(' ', '\n') + '\n')
    shutil.rmtree(work)
    print('made', name)
