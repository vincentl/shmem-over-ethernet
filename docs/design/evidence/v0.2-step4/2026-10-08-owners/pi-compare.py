import re, statistics, sys, glob, os
D = sys.argv[1]
def bench(tree, shape):
    lines = {}
    for i in (1, 2, 3):
        for line in open(f'{D}/bench-4x{shape}-{tree}-{i}.txt'):
            m = re.match(r'(latency|bandwidth|atomic) (.*?) (median_us|mib_per_s)=([\d.]+)', line)
            if not m: continue
            key = f'{m.group(1)} {m.group(2)}'
            if 'where=local' in key: continue
            lines.setdefault(key, []).append(float(m.group(4)))
    return lines
for shape in (4, 1):
    b, t = bench('base', shape), bench('tip', shape)
    print(f'== osoe-bench 4 x {shape} (remote lines; median of three runs, base -> owners, ratio)')
    worst = 0
    for k in b:
        mb, mt = statistics.median(b[k]), statistics.median(t[k])
        r = mt / mb
        print(f'  {k:62s} {mb:10.1f} {mt:10.1f}  {r:5.3f}   base {b[k]}  tip {t[k]}')
for tree in ('base', 'tip'):
    for i in (1, 2, 3):
        s = open(f'{D}/stream-{tree}-{i}.txt').read()
        snd = re.search(r'sender .*mib_per_s=([\d.]+) agent_cpu_s=([\d.]+) agent_cpu_s_per_gib=([\d.]+)', s)
        rcv = re.search(r'receiver .*agent_cpu_s=([\d.]+) agent_busy=([\d.]+)', s)
        h = open(f'{D}/hol-{tree}-{i}.txt').read()
        hs = re.search(r'phase=stream .*median_us=(\d+) p90_us=(\d+) p99_us=(\d+)', h)
        ha = re.search(r'phase=alone .*median_us=(\d+)', h)
        hm = open(f'{D}/hol-many-{tree}-{i}.txt').read()
        hms = [int(x) for x in re.findall(r'pe=\d+ fetch_adds=\d+ median_us=(\d+)', hm)]
        print(f'{tree} {i}: stream {snd.group(1)} MiB/s, sender agent {snd.group(3)} cpu-s/GiB, receiver agent {rcv.group(1)} s busy {rcv.group(2)}; '
              f'hol alone {ha.group(1)} us, behind stream median {hs.group(1)} p90 {hs.group(2)} p99 {hs.group(3)}; hol-many medians {hms}')
