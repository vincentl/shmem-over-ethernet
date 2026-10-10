# SPDX-License-Identifier: LGPL-3.0-or-later
"""v0.2 step 4 part 2 on the Pi: pi-compare.sh's outputs, base (Part 1's tree)
against tip (the control wire), three alternating runs of each.
usage: python3 pi-compare.py OUTDIR"""
import re, statistics, sys

D = sys.argv[1]
RUNS = (1, 2, 3)


def text(name):
    return open(f'{D}/{name}.txt').read()


def bench(tree, shape):
    lines = {}
    for i in RUNS:
        for line in text(f'bench-4x{shape}-{tree}-{i}').splitlines():
            m = re.match(r'(latency|bandwidth|atomic) (.*?) (median_us|mib_per_s)=([\d.]+)', line)
            if not m or 'where=local' in m.group(2):
                continue
            lines.setdefault(f'{m.group(1)} {m.group(2)}', []).append(float(m.group(4)))
    return lines


for shape in (4, 1):
    b, t = bench('base', shape), bench('tip', shape)
    print(f'== osoe-bench 4 x {shape} (remote lines; median of three runs, base -> tip, ratio)')
    for k in b:
        mb, mt = statistics.median(b[k]), statistics.median(t[k])
        print(f'  {k:62s} {mb:10.1f} {mt:10.1f}  {mt / mb:5.3f}   base {b[k]}  tip {t[k]}')

print('== a barrier of PEs 1 and 3 (one a node), alone and behind PE 0\'s 1 MiB put '
      'stream to PE 2 (barrier-behind.c, 2,000 syncs a phase; microseconds)')
for tree in ('base', 'tip'):
    for i in RUNS:
        s = text(f'barrier-{tree}-{i}')
        out = []
        for phase in ('alone', 'stream'):
            m = re.search(rf'phase={phase} syncs=\d+ median_us=(\d+) p90_us=(\d+) p99_us=(\d+) '
                          rf'max_us=(\d+) stream_mib_per_s=(\d+)', s)
            out.append(f'{phase} median {m.group(1)} p90 {m.group(2)} p99 {m.group(3)} '
                       f'max {m.group(4)}' + (f' (stream {m.group(5)} MiB/s)' if phase == 'stream'
                                              else '') if m else f'{phase} MISSING')
        print(f'  {tree} {i}: ' + '; '.join(out))

print('== the 1 MiB put stream with each agent\'s CPU, and a fetch-add behind it (hol.c)')
for tree in ('base', 'tip'):
    for i in RUNS:
        s = text(f'stream-{tree}-{i}')
        snd = re.search(r'sender .*mib_per_s=([\d.]+) agent_cpu_s=([\d.]+) '
                        r'agent_cpu_s_per_gib=([\d.]+)', s)
        rcv = re.search(r'receiver .*agent_cpu_s=([\d.]+) agent_busy=([\d.]+)', s)
        h = text(f'hol-{tree}-{i}')
        hs = re.search(r'phase=stream .*median_us=(\d+) p90_us=(\d+) p99_us=(\d+)', h)
        ha = re.search(r'phase=alone .*median_us=(\d+)', h)
        print(f'  {tree} {i}: stream {snd.group(1)} MiB/s, sender agent {snd.group(3)} '
              f'cpu-s/GiB, receiver agent {rcv.group(1)} s busy {rcv.group(2)}; fetch-add alone '
              f'{ha.group(1)} us, behind the stream median {hs.group(1)} p90 {hs.group(2)} '
              f'p99 {hs.group(3)}')

print('== the mailbox (osoe-pacer, 4 x 2, 20 s): messages a second')
for tree in ('base', 'tip'):
    rates = []
    for i in RUNS:
        m = re.search(r'summary mailbox .*messages_per_s=(\d+)', text(f'mailbox-{tree}-{i}'))
        rates.append(int(m.group(1)) if m else None)
    print(f'  {tree}: {rates}')

print('== start-up: shmem_init, ms (startup.c; PE 0\'s and the slowest PE\'s)')
for shape in (1, 4):
    for tree in ('base', 'tip'):
        runs = []
        for i in RUNS:
            m = re.search(r'init_ms_pe0=([\d.]+) init_ms_slowest=([\d.]+)',
                          text(f'startup-4x{shape}-{tree}-{i}'))
            runs.append(f'{m.group(1)}/{m.group(2)}' if m else 'MISSING')
        print(f'  4 x {shape} {tree}: {runs}')
