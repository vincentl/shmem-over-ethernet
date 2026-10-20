#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""T46's report on runs of t46-series.sh.

    t46-report.py --header                   the names of the brief line's columns
    t46-report.py --brief RUN_DIRECTORY...   one line for each run
    t46-report.py RUN_DIRECTORY...           the lines, then for each run its late leads and what
                                             moved, then the nodes against what moved in them
    t46-report.py --series OUTPUT            a line for each series (a directory of runs) of OUTPUT
    t46-report.py --nstat RUN_DIRECTORY...   node 0's counters in the snapshot after the job against
                                             what nstat -asz printed in its namespace right after

A run directory holds before.json.gz and after.json.gz (t46-snapshot.py), err.txt
(the job's standard error, with SHMEM_DEBUG's "connected its mesh in" lines),
job.txt and tier3-status.txt, and, if it was captured, capture.txt.gz.

A lead is late when it connected its mesh LATE_US after the median lead. Of the 4,496
leads in the runs where no frame was lost on purpose (base but its first run, which was
slow throughout, capture, ab-default, ab-backlog, static and smoke), 4,451 were within
190 ms of their run's median and 45 were 410 to 968 ms behind it; none were between.

The brief line, a column each:
  status           tier3.sh's exit status       job_ms      the job, from just before to just after
  leads            lead lines found             median, max the leads' mesh times, ms
  late             leads that are late          cpu%, sirq% the host's busy and softirq share of the job
  dropped          softnet_stat's dropped, all CPUs;  squeeze  its time_squeeze
  link_tx, link_rx link0's tx_dropped and rx_dropped, summed over the nodes
  port_tx, port_rx the same, summed over the bridge's ports
  resolve_failed, unresolved_discards   arp_cache's res_failed and unresolved_discards
  syn_retrans      TcpExtTCPSynRetrans          timeouts    TcpExtTCPTimeouts
  fast_retrans     TcpExtTCPFastRetrans         attempt_fails  TcpAttemptFails (the start-up's candidate ports, tried in turn, are refused: 30 to 1,900 a run)
  retrans          TcpRetransSegs - which is nearly all tail loss probes (TcpExtTCPLossProbes,
                   TcpExtDelayedACKLost): 2,500 to 4,000 a run, lost frames or none
  listen_overflows, listen_drops   TcpExtListenOverflows and TcpExtListenDrops
  netem            packets netem dropped (tier 3's lossy shape; none on the others)
Everything is summed over the nodes' namespaces.
"""

import gzip
import json
import os
import re
import statistics
import sys

LATE_US = 400000

LOSS_NAMES = re.compile(
    r"Retrans|Timeout|Drop|Overflow|Fail|Discard|NoRoute|Err|Invalid|Lost|Abort|Reset|Cookie|"
    r"Challenge|Spurious|Reorder|Collapse|Prune|OFO|Rst|Loss")

MESH_LINE = re.compile(r"node (\d+) connected its mesh in (\d+) us: (\d+) wires answered and (\d+) called")

TX_DROPPED, RX_DROPPED = 11, 3          # in /proc/net/dev's sixteen columns

NAME_WIDTH = 26                         # of a run's name in the brief line

COLUMNS = ["status", "job_ms", "leads", "median", "max", "late", "cpu%", "sirq%", "dropped",
           "squeeze", "link_tx", "link_rx", "port_tx", "port_rx", "resolve_failed",
           "unresolved_discards", "syn_retrans", "timeouts", "fast_retrans", "retrans", "attempt_fails",
           "listen_overflows", "listen_drops", "netem"]


def load(path):
    with gzip.open(path, "rt") as handle:
        return json.load(handle)


class Run:
    def __init__(self, directory):
        self.directory = directory.rstrip("/")
        self.name = "/".join(self.directory.split("/")[-2:])
        self.status = self.read_text("tier3-status.txt").strip() or "?"
        self.job_ms = None
        job = self.read_text("job.txt")
        started = re.search(r"started_ns=(\d+)", job)
        finished = re.search(r"finished_ns=(\d+)", job)
        if started and finished:
            self.job_ms = (int(finished.group(1)) - int(started.group(1))) // 1000000
        self.mesh = {}
        self.notices = 0
        for line in self.read_text("err.txt").splitlines():
            found = MESH_LINE.search(line)
            if found:
                self.mesh[int(found.group(1))] = int(found.group(2))
            elif "has waited" in line:
                self.notices += 1
        self.before = self.after = None
        try:
            self.before = load(os.path.join(self.directory, "before.json.gz"))
            self.after = load(os.path.join(self.directory, "after.json.gz"))
        except (OSError, ValueError):
            self.before = self.after = None
        self.nodes = self.after["nodes"] if self.after else 0
        self.counters = [self.node_delta(node) for node in range(self.nodes)]

    def read_text(self, name):
        """A file of the run's as text: plain, or gzipped (the copies kept in the repository are)."""
        path = os.path.join(self.directory, name)
        try:
            if os.path.exists(path + ".gz"):
                with gzip.open(path + ".gz", "rt") as handle:
                    return handle.read()
            with open(path) as handle:
                return handle.read()
        except OSError:
            return ""

    def node_delta(self, node):
        """What moved in one node's namespace, by the names nstat gives and a few of our own."""
        before, after = self.before["node"][node], self.after["node"][node]
        if "error" in before or "error" in after:
            return {"snapshot_error": 1}
        names = set(before["counters"]) | set(after["counters"])
        delta = {name: after["counters"].get(name, 0) - before["counters"].get(name, 0)
                 for name in names}
        delta = {name: value for name, value in delta.items() if value}
        for column, key in ((TX_DROPPED, "link_tx_dropped"), (RX_DROPPED, "link_rx_dropped")):
            was, now = before["link0"], after["link0"]
            value = (now[column] if now else 0) - (was[column] if was else 0)
            if value:
                delta[key] = value
        dropped = after["netem"].get("dropped", 0) - before["netem"].get("dropped", 0)
        if dropped:
            delta["netem_dropped"] = dropped
        return delta

    def total(self, name):
        return sum(delta.get(name, 0) for delta in self.counters)

    def median(self):
        return statistics.median(self.mesh.values()) if self.mesh else 0

    def late(self):
        limit = self.median() + LATE_US
        return sorted(node for node, us in self.mesh.items() if us >= limit)

    def softnet(self, column):
        if not self.after:
            return 0
        return sum(now[column] - was[column] for now, was in
                   zip(self.after["host"]["softnet_stat"], self.before["host"]["softnet_stat"]))

    def arp(self, name):
        if not self.after:
            return 0
        return sum(now.get(name, 0) - was.get(name, 0) for now, was in
                   zip(self.after["host"]["arp_cache"], self.before["host"]["arp_cache"]))

    def ports(self, column):
        if not self.after:
            return 0
        before = self.before["switch"].get("ports", {})
        return sum(now[column] - before.get(name, [0] * 16)[column]
                   for name, now in self.after["switch"].get("ports", {}).items())

    def cpu_share(self):
        """(busy %, softirq %) of the host's CPUs over the job, from /proc/stat's "cpu" line."""
        if not self.after:
            return 0.0, 0.0
        now, was = self.after["host"]["stat"]["cpu"], self.before["host"]["stat"]["cpu"]
        change = [a - b for a, b in zip(now, was)]
        total = sum(change[:8]) or 1                    # guest time is in user's already
        idle = change[3] + change[4]
        return 100.0 * (total - idle) / total, 100.0 * (change[5] + change[6]) / total

    def summary(self):
        busy, softirq = self.cpu_share()
        values = sorted(self.mesh.values())
        return {
            "status": self.status, "job_ms": self.job_ms if self.job_ms is not None else -1,
            "leads": len(values), "median": int(self.median() // 1000),
            "max": (values[-1] // 1000) if values else 0, "late": len(self.late()),
            "cpu%": round(busy), "sirq%": round(softirq),
            "dropped": self.softnet(1), "squeeze": self.softnet(2),
            "link_tx": self.total("link_tx_dropped"), "link_rx": self.total("link_rx_dropped"),
            "port_tx": self.ports(TX_DROPPED), "port_rx": self.ports(RX_DROPPED),
            "resolve_failed": self.arp("res_failed"),
            "unresolved_discards": self.arp("unresolved_discards"),
            "syn_retrans": self.total("TcpExtTCPSynRetrans"), "timeouts": self.total("TcpExtTCPTimeouts"),
            "fast_retrans": self.total("TcpExtTCPFastRetrans"),
            "retrans": self.total("TcpRetransSegs"), "attempt_fails": self.total("TcpAttemptFails"),
            "listen_overflows": self.total("TcpExtListenOverflows"),
            "listen_drops": self.total("TcpExtListenDrops"), "netem": self.total("netem_dropped"),
        }


def header_line(width):
    return "%-*s " % (width, "run") + " ".join(COLUMNS)


def brief_line(run, width):
    summary = run.summary()
    return "%-*s " % (width, run.name) + " ".join("%*s" % (len(name), summary[name])
                                                  for name in COLUMNS)


def moved_in(delta):
    return {name: value for name, value in sorted(delta.items())
            if LOSS_NAMES.search(name) or name.startswith(("link_", "netem"))
            or name in ("TcpActiveOpens", "TcpPassiveOpens")}


def run_report(run):
    if not run.mesh:
        return ["%s: no lead lines" % run.name]
    out = []
    slowest = sorted(run.mesh.items(), key=lambda item: -item[1])[:10]
    out.append("%s: the slowest ten leads, node:ms: %s; median %d ms" % (
        run.name, " ".join("%d:%d" % (node, us // 1000) for node, us in slowest),
        run.median() // 1000))
    late = run.late()
    out.append("  late (median + %d ms or more): %d leads%s" % (
        LATE_US // 1000, len(late), (": " + " ".join(str(node) for node in late)) if late else ""))
    for node in late:
        out.append("  node %-3d %6d ms  %s" % (node, run.mesh[node] // 1000, " ".join(
            "%s=%d" % item for item in moved_in(run.counters[node]).items())))
    sums = {}
    for delta in run.counters:
        for name, value in moved_in(delta).items():
            sums[name] = sums.get(name, 0) + value
    if sums:
        out.append("  summed over the nodes: " + " ".join("%s=%d" % item for item in sorted(sums.items())))
    if run.after:
        arp = {name: run.arp(name) for name in (
            "allocs", "destroys", "lookups", "hits", "res_failed", "rcv_probes_mcast",
            "rcv_probes_ucast", "forced_gc_runs", "unresolved_discards", "table_fulls")}
        out.append("  arp_cache: " + " ".join("%s=%d" % item for item in arp.items()))
        out.append("  softnet per CPU, dropped,squeeze: " + " ".join(
            "%d,%d" % (now[1] - was[1], now[2] - was[2]) for now, was in
            zip(run.after["host"]["softnet_stat"], run.before["host"]["softnet_stat"])))
        out.append("  neighbour entries after, node 0: %s" % json.dumps(run.after["node"][0]["neighbours"]))
    if run.notices:
        out.append("  %d \"has waited\" notices in err.txt" % run.notices)
    return out


INDICATORS = [
    ("TcpExtTCPSynRetrans > 0", lambda d: d.get("TcpExtTCPSynRetrans", 0) > 0),
    ("TcpExtTCPTimeouts > 0", lambda d: d.get("TcpExtTCPTimeouts", 0) > 0),
    ("TcpRetransSegs > 0", lambda d: d.get("TcpRetransSegs", 0) > 0),
    ("TcpAttemptFails > 0", lambda d: d.get("TcpAttemptFails", 0) > 0),
    ("link0 tx_dropped > 0", lambda d: d.get("link_tx_dropped", 0) > 0),
    ("link0 rx_dropped > 0", lambda d: d.get("link_rx_dropped", 0) > 0),
    ("ListenOverflows or Drops > 0",
     lambda d: d.get("TcpExtListenOverflows", 0) + d.get("TcpExtListenDrops", 0) > 0),
]


def node_table(runs):
    out = ["The nodes of %d runs, late or on time, against what moved in their namespaces:" % len(runs),
           "%-30s %10s %10s %10s %10s" % ("", "late, did", "late, not", "on time, did", "on time, not")]
    for name, predicate in INDICATORS:
        counts = [0, 0, 0, 0]
        for run in runs:
            late = set(run.late())
            for node, delta in enumerate(run.counters):
                if node in run.mesh:
                    counts[(0 if node in late else 2) + (0 if predicate(delta) else 1)] += 1
        out.append("%-30s %10d %10d %12d %12d" % ((name,) + tuple(counts)))
    return out


SERIES_COLUMNS = ["runs", "with_late", "late_leads", "max_ms_least", "max_ms_median", "max_ms_most",
                  "median_ms", "runs_dropped", "dropped", "link_tx", "link_rx", "port_tx", "port_rx",
                  "attempt_fails", "syn_retrans", "timeouts", "fast_retrans", "retrans", "netem"]


def series_lines(output):
    """One line for each series under output: its runs added up."""
    names = sorted(name for name in os.listdir(output)
                   if os.path.isdir(os.path.join(output, name)) and
                   any(entry.startswith("run-") for entry in os.listdir(os.path.join(output, name))))
    width = max([len(name) for name in names] + [6])
    lines = ["%-*s " % (width, "series") + " ".join(SERIES_COLUMNS)]
    for name in names:
        directory = os.path.join(output, name)
        runs = [Run(os.path.join(directory, entry)) for entry in sorted(os.listdir(directory))
                if entry.startswith("run-")]
        summaries = [run.summary() for run in runs if run.mesh]
        if not summaries:
            lines.append("%-*s  %d runs, no lead lines" % (width, name, len(runs)))
            continue
        most = sorted(summary["max"] for summary in summaries)
        row = {
            "runs": len(summaries), "with_late": sum(1 for summary in summaries if summary["late"]),
            "late_leads": sum(summary["late"] for summary in summaries),
            "max_ms_least": most[0], "max_ms_median": most[len(most) // 2], "max_ms_most": most[-1],
            "median_ms": sorted(summary["median"] for summary in summaries)[len(summaries) // 2],
            "runs_dropped": sum(1 for summary in summaries if summary["dropped"]),
        }
        for column, key in (("dropped", "dropped"), ("link_tx", "link_tx"), ("link_rx", "link_rx"),
                            ("port_tx", "port_tx"), ("port_rx", "port_rx"),
                            ("attempt_fails", "attempt_fails"), ("syn_retrans", "syn_retrans"),
                            ("timeouts", "timeouts"), ("fast_retrans", "fast_retrans"),
                            ("retrans", "retrans"), ("netem", "netem")):
            row[column] = sum(summary[key] for summary in summaries)
        lines.append("%-*s " % (width, name) + " ".join("%*s" % (len(column), row[column])
                                                         for column in SERIES_COLUMNS))
    return lines


def nstat_check(directory):
    """Compare node 0's counters in after.json.gz with nstat-node0.txt, as lines of text.

    nstat also reads /proc/net/snmp6, which the snapshot does not, and leaves out the
    configuration values /proc/net/snmp has among its counters; everything else must agree.
    """
    after = load(os.path.join(directory, "after.json.gz"))["node"][0]["counters"]
    shown = {}
    with open(os.path.join(directory, "nstat-node0.txt")) as handle:
        for line in handle:
            fields = line.split()
            if len(fields) >= 2 and not line.startswith("#"):
                shown[fields[0]] = int(fields[1])
    differ = {name: (after.get(name, 0), value) for name, value in shown.items()
              if after.get(name, 0) != value}
    from_snmp6 = {name for name in differ if name.startswith(("Icmp6", "Ip6", "Udp6", "UdpLite6"))}
    missing = sorted(name for name in after if name not in shown)
    lines = ["%s: nstat printed %d counters, the snapshot kept %d nonzero ones" % (
        directory, len(shown), len(after)),
        "  %d counters differ; %d of them are IPv6 counters from /proc/net/snmp6 (%s)" % (
            len(differ), len(from_snmp6), " ".join(sorted(from_snmp6))),
        "  differing and not IPv6: %s" % (" ".join("%s snapshot %d nstat %d" % ((name,) + differ[name])
                                                  for name in sorted(set(differ) - from_snmp6)) or "none"),
        "  in the snapshot and not in nstat's output (settings, not counters): %s" % (
            " ".join(missing) or "none")]
    return "\n".join(lines)


def main():
    arguments = sys.argv[1:]
    if arguments[:1] == ["--nstat"]:
        for directory in arguments[1:]:
            print(nstat_check(directory))
        return
    if len(arguments) == 2 and arguments[0] == "--series":
        for line in series_lines(arguments[1]):
            print(line)
        return
    if arguments == ["--header"]:
        print(header_line(NAME_WIDTH))
        return
    brief = bool(arguments) and arguments[0] == "--brief"
    runs = [Run(directory) for directory in (arguments[1:] if brief else arguments)]
    if not brief:
        print(header_line(NAME_WIDTH))
    for run in runs:
        print(brief_line(run, NAME_WIDTH))
    if not brief:
        for run in runs:
            print()
            for line in run_report(run):
                print(line)
        print()
        for line in node_table(runs):
            print(line)


if __name__ == "__main__":
    main()
