#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# load-sampler.py ROOT_PID INTERVAL LOGFILE
# T4's record of whether the VM was loaded by anything but T4. Every INTERVAL seconds appends a line:
# the time, the 1-minute load average, the cores the whole machine used (/proc/stat), the cores the
# job used (the live descendants of ROOT_PID) and the cores the rest used, with the live processes of
# the rest above 0.10 core in that interval. Short-lived processes that start and end between two
# samples are not seen individually, but the machine-wide figure still has them.
import os, sys, time

root, interval, log = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
hz = os.sysconf("SC_CLK_TCK")


def stat_total():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    # user nice system idle iowait irq softirq steal
    return v[0] + v[1] + v[2] + v[5] + v[6] + v[7]


def snapshot():
    procs = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % name) as f:
                s = f.read()
            r = s.rindex(")")
            comm = s[s.index("(") + 1:r]
            rest = s[r + 2:].split()
            procs[int(name)] = (int(rest[1]), comm, int(rest[11]) + int(rest[12]))
        except (OSError, ValueError, IndexError):
            continue
    return procs


def cmdline(pid, comm):
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            c = f.read().replace(b"\0", b" ").decode("utf-8", "replace").strip()
        return c[:60] if c else "[" + comm + "]"
    except OSError:
        return comm


def descendants(procs, top):
    kids = {}
    for pid, (ppid, _, _) in procs.items():
        kids.setdefault(ppid, []).append(pid)
    out, todo = set(), [top]
    while todo:
        p = todo.pop()
        if p in out:
            continue
        out.add(p)
        todo.extend(kids.get(p, []))
    return out


prev_t, prev_total, prev = time.time(), stat_total(), snapshot()
with open(log, "a", buffering=1) as out:
    while True:
        time.sleep(interval)
        if not os.path.exists("/proc/%d" % root):
            out.write("%s sampler ends: root %d gone\n" % (time.strftime("%H:%M:%S"), root))
            break
        now_t, now_total, now = time.time(), stat_total(), snapshot()
        dt = now_t - prev_t
        mine = descendants(now, root)
        machine = (now_total - prev_total) / (hz * dt)
        job = 0.0
        foreign, foreign_list = 0.0, []
        for pid, (ppid, comm, ticks) in now.items():
            if pid in prev:
                d = ticks - prev[pid][2]
                if d <= 0:
                    continue
            else:
                continue
            cores = d / (hz * dt)
            if pid in mine or pid == os.getpid():
                job += cores
            else:
                foreign += cores
                if cores >= 0.10:
                    foreign_list.append((cores, pid, cmdline(pid, comm)))
        foreign_list.sort(reverse=True)
        with open("/proc/loadavg") as f:
            load = f.read().split()[0]
        top = " ".join("%d:%.2f:%s" % (pid, c, cmd) for c, pid, cmd in foreign_list[:4])
        out.write("%s load1=%s machine=%.2f job=%.2f others=%.2f %s\n" % (
            time.strftime("%H:%M:%S"), load, machine, job, foreign, top))
        prev_t, prev_total, prev = now_t, now_total, now
