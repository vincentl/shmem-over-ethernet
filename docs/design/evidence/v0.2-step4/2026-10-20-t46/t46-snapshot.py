#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""One snapshot of the counters T46 reads around a tier 3 job.

    t46-snapshot.py PREFIX NODES OUTPUT.json.gz

Run as root between tier3.sh's set-up and its tear-down, PREFIX being the
namespace prefix tier3.sh hands its launcher (-N). It records

  - the host's: /proc/net/softnet_stat, /proc/net/stat/arp_cache (one table for
    every namespace), the CPUs' times, /proc/softirqs and the load average;
  - the switch namespace's: /proc/net/dev, a line for every bridge port;
  - for each node namespace PREFIX<i>: /proc/net/snmp and /proc/net/netstat -
    the counters nstat prints, under the names it prints them - link0's line of
    /proc/net/dev, how many neighbour entries are complete and how many not,
    and the packets netem has dropped, which tc reports.

Only counters that are not zero are kept. t46-report.py subtracts the snapshot
taken before a job from the one taken after it.
"""

import gzip
import json
import os
import re
import subprocess
import sys
import time

NETEM_LINE = re.compile(r"Sent (\d+) bytes (\d+) pkt \(dropped (\d+), overlimits (\d+) requeues (\d+)\)")

DEVICE_COLUMNS = [
    "rx_bytes", "rx_packets", "rx_errors", "rx_dropped", "rx_fifo", "rx_frame", "rx_compressed",
    "rx_multicast", "tx_bytes", "tx_packets", "tx_errors", "tx_dropped", "tx_fifo",
    "tx_collisions", "tx_carrier", "tx_compressed",
]


def read_pairs(path):
    """/proc/net/snmp and /proc/net/netstat: a header line, then a line of values, per group."""
    counters = {}
    with open(path) as handle:
        lines = handle.read().splitlines()
    for header, values in zip(lines[0::2], lines[1::2]):
        group, _, names = header.partition(":")
        _, _, numbers = values.partition(":")
        for name, number in zip(names.split(), numbers.split()):
            value = int(number)
            if value:
                counters[group + name] = value
    return counters


def read_devices(path):
    """/proc/net/dev: every device that has counted anything, as {name: [16 numbers]}."""
    devices = {}
    with open(path) as handle:
        for line in handle.read().splitlines()[2:]:
            name, _, numbers = line.partition(":")
            values = [int(number) for number in numbers.split()]
            if any(values):
                devices[name.strip()] = values
    return devices


def read_neighbours():
    complete = incomplete = 0
    with open("/proc/net/arp") as handle:
        for line in handle.read().splitlines()[1:]:
            fields = line.split()
            if len(fields) >= 3:
                if int(fields[2], 16) & 2:
                    complete += 1
                else:
                    incomplete += 1
    return {"complete": complete, "incomplete": incomplete}


def read_netem():
    output = subprocess.run(["tc", "-s", "qdisc", "show", "dev", "link0"], capture_output=True,
                            text=True).stdout
    found = NETEM_LINE.search(output)
    if not found:
        return {}
    sent_bytes, sent_packets, dropped, overlimits, requeues = (int(v) for v in found.groups())
    return {"sent_bytes": sent_bytes, "sent_packets": sent_packets, "dropped": dropped,
            "overlimits": overlimits, "requeues": requeues}


def read_node():
    counters = read_pairs("/proc/net/snmp")
    counters.update(read_pairs("/proc/net/netstat"))
    devices = read_devices("/proc/net/dev")
    return {"counters": counters, "link0": devices.get("link0", []),
            "neighbours": read_neighbours(), "netem": read_netem()}


def read_switch():
    return {"ports": read_devices("/proc/net/dev")}


def in_namespace(path, function):
    """function()'s result, read from a child that has entered the namespace at path.

    A child of its own, so that /proc/self/net is looked up afresh in each namespace.
    """
    read_end, write_end = os.pipe()
    pid = os.fork()
    if pid == 0:
        status = 1
        try:
            os.close(read_end)
            descriptor = os.open(path, os.O_RDONLY)
            os.setns(descriptor, os.CLONE_NEWNET)
            os.close(descriptor)
            payload = memoryview(json.dumps(function()).encode())
            while payload:
                payload = payload[os.write(write_end, payload):]
            status = 0
        except BaseException as problem:  # noqa: B036 - the child must report and leave
            try:
                os.write(write_end, json.dumps({"error": repr(problem)}).encode())
            except OSError:
                pass
        finally:
            os._exit(status)
    os.close(write_end)
    chunks = []
    while True:
        chunk = os.read(read_end, 65536)
        if not chunk:
            break
        chunks.append(chunk)
    os.close(read_end)
    os.waitpid(pid, 0)
    try:
        return json.loads(b"".join(chunks))
    except ValueError:
        return {"error": "no output"}


def read_host():
    host = {}
    with open("/proc/net/softnet_stat") as handle:
        host["softnet_stat"] = [[int(field, 16) for field in line.split()]
                                for line in handle.read().splitlines()]
    with open("/proc/net/stat/arp_cache") as handle:
        lines = handle.read().splitlines()
    names = lines[0].split()
    host["arp_cache"] = [dict(zip(names, (int(field, 16) for field in line.split())))
                         for line in lines[1:]]
    with open("/proc/stat") as handle:
        host["stat"] = {line.split()[0]: [int(field) for field in line.split()[1:]]
                        for line in handle.read().splitlines()
                        if line.startswith("cpu") or line.startswith(("ctxt", "intr", "softirq"))}
    with open("/proc/softirqs") as handle:
        rows = handle.read().splitlines()
    host["softirqs"] = {row.split(":")[0].strip(): [int(field) for field in row.split(":")[1].split()]
                        for row in rows[1:]}
    with open("/proc/loadavg") as handle:
        host["loadavg"] = handle.read().strip()
    return host


def main():
    prefix, nodes, output = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    snapshot = {"realtime_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns(),
                "prefix": prefix, "nodes": nodes}
    snapshot["host"] = read_host()
    snapshot["switch"] = in_namespace("/run/netns/%sswitch" % prefix, read_switch)
    snapshot["node"] = [in_namespace("/run/netns/%s%d" % (prefix, node), read_node)
                        for node in range(nodes)]
    snapshot["finished_realtime_ns"] = time.time_ns()
    with gzip.open(output, "wt") as handle:
        json.dump(snapshot, handle, separators=(",", ":"))


if __name__ == "__main__":
    main()
