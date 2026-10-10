#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The fast-link kit's summaries (docs/fast-link.md): give it the output
directory of any section's script, and it prints that section's table - each
case's median over its rounds, with the range beside it.

    summarize.py OUTDIR [OUTDIR...]

A section is known by its files: multistream's, the mailbox's, hol's,
get-pieces', fan-in's, echo-uring's and iperf3's, osoe-bench's and the
start-up probe's. Counters PE 0 printed between "counters start begin" and
"counters end end" (osoe-stat -d -j) are compared field by field, and the
fields whose names say zero_copy, enobufs or rearm are shown where they moved.
"""
import json
import os
import re
import statistics
import sys
from collections import defaultdict


def number_fields(line):
    return {k: float(v) for k, v in re.findall(r"(\w+)=(-?[0-9.]+)\b", line)}


def spread(values):
    if not values:
        return "-"
    m = statistics.median(values)
    if len(values) == 1:
        return f"{m:.1f}"
    return f"{m:.1f} ({min(values):.1f}-{max(values):.1f})"


def name_parts(name):
    """'put-1m-pes8-h2-r3' -> ('put-1m', {'pes': 8, 'h': 2, 'r': 3})"""
    stem = os.path.splitext(name)[0]
    parts = {}
    for key, value in re.findall(r"-(pes|h|r|zone|buffers)(\d+)", stem):
        parts[key] = int(value)
    base = re.sub(r"-(pes|h|r|zone|buffers)\d+", "", stem)
    return base, parts


def counters(text, which):
    """The JSON osoe-stat printed between "counters WHICH begin" and "counters WHICH
    end": an object a line, one for each of the job's segments it found on PE 0's
    machine - node 0's alone on a cluster, every logical node's on one machine -
    of which node 0's, named /osoe.<job>.<step>.<node>.<session>, is PE 0's."""
    m = re.search(rf"^counters {which} begin\n(.*?)^counters {which} end$", text, re.S | re.M)
    if not m:
        return None
    first = None
    for line in m.group(1).splitlines():
        try:
            record = json.loads(line)
        except ValueError:
            continue
        if first is None:
            first = record
        name = re.search(r"\.(\d+)\.(\d+)$", str(record.get("segment", "")))
        if name and name.group(1) == "0":
            return record
    return first


def flatten(value, prefix=""):
    out = {}
    if isinstance(value, dict):
        for k, v in value.items():
            out.update(flatten(v, f"{prefix}.{k}" if prefix else str(k)))
    elif isinstance(value, list):
        for i, v in enumerate(value):
            out.update(flatten(v, f"{prefix}[{i}]"))
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        out[prefix] = value
    return out


def moved_counters(text):
    start, end = counters(text, "start"), counters(text, "end")
    if start is None or end is None:
        return {}
    a, b = flatten(start), flatten(end)
    wanted = re.compile(r"zero_copy|enobufs|rearm")
    totals = defaultdict(float)
    for key, value in b.items():
        leaf = re.sub(r"\[\d+\]", "", key).split(".")[-1]
        if wanted.search(key) and key in a:
            totals[leaf] += value - a[key]
    return {k: v for k, v in totals.items() if v}


def summarize(directory):
    files = sorted(f for f in os.listdir(directory) if f.endswith((".txt", ".json")))
    multistream = defaultdict(lambda: defaultdict(list))   # (base, pes, h, zone) -> field -> values
    mailbox = defaultdict(list)
    hol = defaultdict(lambda: defaultdict(list))
    pieces = defaultdict(lambda: defaultdict(list))         # zone -> bytes -> [(put, get, nbi)]
    fan_in = defaultdict(lambda: defaultdict(list))
    moved = defaultdict(lambda: defaultdict(list))
    echo = {}
    iperf = {}
    startup = defaultdict(list)
    other = []
    for f in files:
        path = os.path.join(directory, f)
        text = open(path, errors="replace").read()
        base, parts = name_parts(f)
        key = (base, parts.get("pes"), parts.get("h"), parts.get("zone"), parts.get("buffers"))
        if f.endswith(".json") and f.startswith("iperf3-"):
            try:
                data = json.loads(text)
                k = int(re.search(r"iperf3-(\d+)", f).group(1))
                iperf[k] = data["end"]["sum_received"]["bits_per_second"] / 8 / (1 << 20)
            except (ValueError, KeyError, AttributeError):
                pass
            continue
        for line in text.splitlines():
            if line.startswith("multistream job "):
                fields = number_fields(line)
                for k in ("mib_per_s", "progress_cpu_s_per_gib"):
                    multistream[key][k].append(fields[k])
            elif line.startswith("summary mailbox "):
                mailbox[key].append(number_fields(line)["messages_per_s"])
            elif line.startswith("hol phase="):
                fields = number_fields(line)
                phase = re.search(r"phase=(\w+)", line).group(1)
                hol[(base, phase)]["median_us"].append(fields["median_us"])
                hol[(base, phase)]["p99_us"].append(fields["p99_us"])
                if phase == "stream":
                    hol[(base, phase)]["stream_mib_per_s"].append(fields["stream_mib_per_s"])
            elif line.startswith("bytes=") and "get_nbi_quiet_us" in line:
                fields = number_fields(line)
                pieces[parts.get("zone")][int(fields["bytes"])].append(
                    (fields["put_quiet_us"], fields["get_us"], fields["get_nbi_quiet_us"]))
            elif line.startswith("fan-in "):
                fan_in[key]["mib_per_s"].append(number_fields(line)["mib_per_s"])
            elif line.startswith("startup "):
                fields = number_fields(line)
                startup[parts.get("pes")].append(fields["init_ms_slowest"])
        for name, value in moved_counters(text).items():
            moved[key][name].append(value)
        m = re.match(r"echo-(\d+)\.txt$", f)
        if m:
            per = defaultdict(list)
            for c, rate in re.findall(r"^connection=(\d+) uring stream_mib_per_s=([0-9.]+)", text, re.M):
                per[c].append(float(rate))
            if per:
                echo[int(m.group(1))] = sum(statistics.median(v) for v in per.values())
        if f.startswith("bench-"):
            other.append((f, [l for l in text.splitlines() if l and not l.startswith("#")][:60]))

    print(f"== {directory}")
    if echo or iperf:
        print("\nthe bare aggregate, MiB/s (echo-uring: each connection's median stream, summed)")
        for k in sorted(set(echo) | set(iperf)):
            e = f"{echo[k]:9.1f}" if k in echo else f"{'-':>9}"
            i = f"{iperf[k]:9.1f}" if k in iperf else f"{'-':>9}"
            print(f"  {k:3d} connections  echo-uring {e}   iperf3 {i}")
    if multistream:
        print("\nmultistream: MiB/s, and the progress threads' CPU seconds a GiB (median, range)")
        for key in sorted(multistream, key=lambda k: tuple(-1 if x is None else x for x in k[1:]) + (k[0],)):
            base, pes, h, zone, buffers = key
            label = f"{base:10s} pes={pes} h={h}" + (f" zone={zone}" if zone else "")
            v = multistream[key]
            print(f"  {label:36s} {spread(v['mib_per_s']):28s} cpu/GiB {spread(v['progress_cpu_s_per_gib'])}")
    if mailbox:
        print("\nmailbox: messages a second (median, range)")
        for key in sorted(mailbox, key=lambda k: (k[1] or 0, k[2] or 0)):
            print(f"  pes={key[1]} h={key[2]}  {spread(mailbox[key])}")
    if hol:
        print("\nhol: a fetch-add's round trip, microseconds, alone and behind the stream; the stream's MiB/s")
        for key in sorted(hol):
            v = hol[key]
            line = f"  {key[0]:18s} {key[1]:6s} median {spread(v['median_us']):22s} p99 {spread(v['p99_us']):22s}"
            if v["stream_mib_per_s"]:
                line += f" stream {spread(v['stream_mib_per_s'])}"
            print(line)
    if pieces:
        print("\nget-pieces: medians of the rounds' medians, microseconds; a get's and a nonblocking get's time over a put's")
        for zone in sorted(pieces, key=lambda z: z or 0):
            print(f"  zone {zone}")
            for size in sorted(pieces[zone]):
                rows = pieces[zone][size]
                put = statistics.median(r[0] for r in rows)
                get = statistics.median(r[1] for r in rows)
                nbi = statistics.median(r[2] for r in rows)
                print(f"    {size:>10d} B  put {put:10.1f}  get {get:10.1f} ({get / put:.2f}x)  nbi {nbi:10.1f} ({nbi / put:.2f}x)")
    if fan_in:
        print("\nfan-in: MiB/s into PE 0")
        for key in sorted(fan_in, key=lambda k: tuple(-1 if x is None else x for x in k[1:])):
            print(f"  pes={key[1]} h={key[2]} buffers={key[4]}  {spread(fan_in[key]['mib_per_s'])}")
    if moved:
        print("\ncounters that moved over the window (PE 0's node; median, range)")
        for key in sorted(moved, key=lambda k: tuple(-1 if x is None else x for x in k[1:]) + (k[0],)):
            fields = ", ".join(f"{n} {spread(v)}" for n, v in sorted(moved[key].items()))
            print(f"  {key[0]} pes={key[1]} h={key[2]} buffers={key[4]}: {fields}")
    if startup:
        print("\nstart-up: the slowest PE's shmem_init, milliseconds")
        for pes in sorted(startup):
            print(f"  pes={pes}  {spread(startup[pes])}")
    for f, lines in other:
        print(f"\n{f}")
        for line in lines:
            print(f"  {line}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for d in sys.argv[1:]:
        summarize(d)
