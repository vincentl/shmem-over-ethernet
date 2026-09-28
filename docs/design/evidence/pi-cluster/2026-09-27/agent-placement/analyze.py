import sys, collections
# sample lines: label time node pid procid tid role cpus psr run_ns wait_ns slices vol invol
def load(path):
    rows = collections.defaultdict(dict)   # label -> (node, tid) -> row
    for line in open(path):
        f = line.split()
        if len(f) != 14: continue
        label, t, node, pid, procid, tid, role, cpus, psr = f[:9]
        run, wait, slices, vol, invol = map(int, f[9:])
        rows[label][(node, tid)] = dict(t=float(t), node=node, procid=procid, role=role, cpus=cpus,
                                        run=run, wait=wait, slices=slices, vol=vol, invol=invol)
    return rows
def window(rows, a, b):
    out = []
    for key, r1 in rows[b].items():
        r0 = rows[a].get(key)
        if not r0: continue
        dt = r1['t'] - r0['t']
        out.append(dict(node=r1['node'], procid=r1['procid'], role=r1['role'], cpus=r1['cpus'],
                        cpu=(r1['run'] - r0['run']) / 1e9 / dt, waitq=(r1['wait'] - r0['wait']) / 1e9 / dt,
                        invol=(r1['invol'] - r0['invol']) / dt, vol=(r1['vol'] - r0['vol']) / dt, dt=dt))
    return out
def summary(name, w):
    by = collections.defaultdict(list)
    for r in w: by[r['role']].append(r)
    print(f"{name}: window {w[0]['dt']:.1f} s")
    print(f"  {'thread':12} {'n':>2} {'cpu share':>9} {'run-queue':>9} {'preempted/s':>11} {'slept/s':>8}  cpus")
    for role in ('lead-main', 'lead-agent', 'main', 'agent'):
        rs = by.get(role, [])
        if not rs: continue
        n = len(rs)
        avg = lambda k: sum(r[k] for r in rs) / n
        cpus = ','.join(sorted(set(r['cpus'] for r in rs)))
        print(f"  {role:12} {n:>2} {avg('cpu'):9.3f} {avg('waitq'):9.3f} {avg('invol'):11.1f} {avg('vol'):8.1f}  {cpus}")
for path in sys.argv[1:]:
    rows = load(path)
    labels = sorted(rows, key=float)
    name = path.split('/')[-1].replace('-samples.txt', '')
    for a, b in zip(labels[::2], labels[1::2]):
        summary(f"{name} [{a}..{b} s]", window(rows, a, b))
