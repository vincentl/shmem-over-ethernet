#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""What T46's capture says about the late connections of a run.

    t46-capture-report.py RUN_DIRECTORY...            the report of each run
    t46-capture-report.py --totals RUN_DIRECTORY...   one line of figures for each run, and their sum

RUN_DIRECTORY holds capture.txt.gz (t46-sniff.py) and err.txt (the job's standard
error, with SHMEM_DEBUG's "connected its mesh in" lines). For each run:

  - the ARP exchanges: requests and replies sent, received, and sent and never
    received (lost between the sender's link0 and the receiver's); the pairs
    that asked twice; every pair that took SLOW_NS or more to be resolved, and
    which frame was lost. A pair is resolved by whichever comes first: the
    reply, a request the other node sent the asker, a request the other node
    broadcast for someone else (the kernel takes the sender's address from any
    ARP frame, and the capture keeps only the frames sent or addressed to a
    node, so it sees these only on the sender's side), or - at the latest - the
    SYN that was waiting for the answer leaving;
  - the TCP handshakes: SYNs, SYN-ACKs and RSTs sent and received; SYNs sent
    again with the same sequence number (the kernel's retransmission); a dial
    made again from a new port (the dial deadline of PROBE_NS in listener.c,
    which closes a socket still waiting for its SYN-ACK and dials afresh);
    every connection that took SLOW_NS or more from its first SYN to its SYN-ACK,
    or that no SYN-ACK ever answered, with the first frame of it that was lost.
    A connection the answering node reset at once (a refused port: the start-up
    tries a lead's candidate ports in turn) is counted and left out;
  - which late leads (t46-report.py's: 0.4 s behind the median lead) those
    slow resolutions and connections account for, as caller or as answerer.

A packet "sent and not received" was lost between the two taps - in the veth
pairs, the bridge or the per-CPU backlog queue. A packet netem drops (tier 3's
lossy shape) is never sent as far as the capture can tell, and a SYN that was
waiting for an ARP reply is not sent until it comes.
"""

import collections
import gzip
import lzma
import os
import re
import statistics
import sys

SLOW_NS = 200000000
LATE_US = 400000
MATCH_NS = 1000000000           # a frame is received within this of being sent, or it was lost
STORIES = 40                    # the stories printed for each run
MESH_LINE = re.compile(r"node (\d+) connected its mesh in (\d+) us")


def open_text(path):
    """path as text, whichever of path, path.gz and path.xz exists (the repository keeps the xz)."""
    for suffix, opener in (("", open), (".gz", gzip.open), (".xz", lzma.open)):
        if os.path.exists(path + suffix):
            return opener(path + suffix, "rt")
    raise OSError("no %s" % path)


def read_events(path):
    events = []
    footer = []
    with open_text(path) as handle:
        for line in handle:
            if line.startswith("#"):
                footer.append(line.strip())
                continue
            fields = line.split()
            time_ns, node, direction, kind = int(fields[0]), int(fields[1]), fields[2], fields[3]
            if kind == "arp":
                events.append((time_ns, node, direction, "arp", fields[4], int(fields[5]), int(fields[6])))
            else:
                source, source_port = fields[5].rsplit(":", 1)
                destination, destination_port = fields[6].rsplit(":", 1)
                events.append((time_ns, node, direction, "tcp", fields[4],
                               (int(source), int(source_port)), (int(destination), int(destination_port)),
                               int(fields[7]), int(fields[8])))
    return events, footer


def pair_up(sent, received):
    """For each time in sent, the first time in received at or after it, within MATCH_NS, or None.

    Both lists are sorted, and a received time answers one sent time at most.
    """
    matched = []
    cursor = 0
    for time_ns in sent:
        while cursor < len(received) and received[cursor] < time_ns:
            cursor += 1
        if cursor < len(received) and received[cursor] - time_ns <= MATCH_NS:
            matched.append(received[cursor])
            cursor += 1
        else:
            matched.append(None)
    return matched


def ms(time_ns):
    return "%.1f" % (time_ns / 1e6)


def times(list_of_times, matched=None):
    if matched is None:
        return ", ".join(ms(t) for t in list_of_times) or "-"
    return ", ".join("%s (%s)" % (ms(t), ("received " + ms(r)) if r is not None else "NOT received")
                     for t, r in zip(list_of_times, matched)) or "-"


class Capture:
    def __init__(self, events):
        self.arp_sent = collections.defaultdict(list)       # (operation, sender, target) -> times
        self.arp_received = collections.defaultdict(list)
        self.syn_sent = collections.defaultdict(list)       # (caller endpoint, answerer endpoint) -> times
        self.syn_received = collections.defaultdict(list)
        self.synack_sent = collections.defaultdict(list)    # (caller endpoint, answerer endpoint) -> times
        self.synack_received = collections.defaultdict(list)
        self.resets = 0
        self.refused = set()            # (caller endpoint, answerer endpoint) answered with an RST
        self.dropped_by_caller = set()  # (caller endpoint, answerer endpoint) the caller sent an RST on
        self.requests_by_sender = collections.defaultdict(list)      # node -> times of its requests
        self.syn_by_pair = collections.defaultdict(list)             # (caller, answerer) -> times
        for event in sorted(events):
            time_ns, node, direction, kind = event[:4]
            if kind == "arp":
                key = (event[4], event[5], event[6])
                (self.arp_sent if direction == "out" else self.arp_received)[key].append(time_ns)
                if direction == "out" and event[4] == "request":
                    self.requests_by_sender[event[5]].append(time_ns)
                continue
            flags, source, destination = event[4], event[5], event[6]
            if flags == "S":
                (self.syn_sent if direction == "out" else self.syn_received)[
                    (source, destination)].append(time_ns)
                if direction == "out":
                    self.syn_by_pair[(source[0], destination[0])].append(time_ns)
            elif flags == "SA":
                (self.synack_sent if direction == "out" else self.synack_received)[
                    (destination, source)].append(time_ns)
            elif "R" in flags:
                self.resets += 1
                if direction == "in":
                    # An RST that reaches the caller: from the answerer, so the key is (destination, source).
                    self.refused.add((destination, source))
                else:
                    # An RST the caller sends: the key is (source, destination). Sent to a SYN-ACK that
                    # comes after the caller has closed its socket and dialled again.
                    self.dropped_by_caller.add((source, destination))

    def arp_counts(self, operation):
        """(sent, received, sent and not received) of the ARP requests or replies."""
        sent = lost = 0
        for key, sent_times in self.arp_sent.items():
            if key[0] == operation:
                sent += len(sent_times)
                lost += sum(1 for r in pair_up(sent_times, self.arp_received.get(key, [])) if r is None)
        received = sum(len(t) for k, t in self.arp_received.items() if k[0] == operation)
        return sent, received, lost

    def arp_totals(self):
        lines = []
        for operation in ("request", "reply"):
            lines.append("arp %s: %d sent, %d received, %d sent and not received" % (
                (operation,) + self.arp_counts(operation)))
        return lines

    def resolution(self, asker, target):
        """(request times, when the asker learned the target's address or None, how) for a pair."""
        requests = self.arp_sent.get(("request", asker, target), [])
        if not requests:
            return requests, None, "no request"
        first = requests[0]
        options = []
        replies = [t for t in self.arp_received.get(("reply", target, asker), []) if t >= first]
        if replies:
            options.append((replies[0], "by the reply"))
        asked = [t for t in self.arp_received.get(("request", target, asker), []) if t >= first]
        if asked:
            options.append((asked[0], "by the target's own request to it"))
        broadcast = [t for t in self.requests_by_sender.get(target, []) if t > first]
        if broadcast:
            options.append((broadcast[0], "by a request the target broadcast for another node"))
        syns = [t for t in self.syn_by_pair.get((asker, target), []) if t >= first]
        if syns:
            options.append((syns[0], "no later than the SYN leaving"))
        if not options:
            return requests, None, "never"
        resolved, how = min(options)
        return requests, resolved, how

    def arp_story(self, asker, target):
        """(the cause, the story's lines) of a pair's ARP resolution."""
        requests, resolved, how = self.resolution(asker, target)
        arrivals = pair_up(requests, self.arp_received.get(("request", asker, target), []))
        replies = self.arp_sent.get(("reply", target, asker), [])
        reply_arrivals = pair_up(replies, self.arp_received.get(("reply", target, asker), []))
        cause = "never resolved" if resolved is None else "resolved " + how
        if requests and arrivals[0] is None:
            cause += ", the first request lost in transit"
        elif replies and reply_arrivals[0] is None:
            cause += ", the first reply lost in transit"
        return cause, ["arp %d -> %d: %s, after %s ms" % (
            asker, target, cause, ms(resolved - requests[0]) if resolved is not None else "-"),
            "    requests sent at %s" % times(requests, arrivals),
            "    replies sent at %s" % times(replies, reply_arrivals)]

    def connection_cause(self, record):
        """The first frame of a connection that was lost, from its SYNs and SYN-ACKs."""
        sent, received = record["syn"], record["syn_received"]
        answered, got_back = record["synack"], record["synack_received"]
        if not sent:
            return "no SYN of it was seen"
        arrivals = pair_up(sent, received)
        if arrivals[0] is None:
            return "the first SYN was lost in transit"
        horizon = sent[1] if len(sent) > 1 else float("inf")
        replies = [t for t in answered if arrivals[0] <= t < horizon]
        if not replies:
            return "the first SYN arrived and no SYN-ACK was sent for it"
        delivered = pair_up(answered, got_back)
        if delivered[answered.index(replies[0])] is None:
            return "the first SYN-ACK was lost in transit"
        return "the first SYN and SYN-ACK both arrived"


def connection_records(capture):
    records = {}
    for name, source in (("syn", capture.syn_sent), ("syn_received", capture.syn_received),
                         ("synack", capture.synack_sent), ("synack_received", capture.synack_received)):
        for key, found in source.items():
            records.setdefault(key, {"syn": [], "syn_received": [], "synack": [], "synack_received": []})[
                name] = sorted(found)
    return records


def analyse(directory):
    directory = directory.rstrip("/")
    try:
        events, footer = read_events(directory + "/capture.txt")
    except OSError:
        return ["%s: no capture" % directory], {}
    mesh = {}
    try:
        with open_text(directory + "/err.txt") as handle:
            for line in handle:
                found = MESH_LINE.search(line)
                if found:
                    mesh[int(found.group(1))] = int(found.group(2))
    except OSError:
        pass
    median = statistics.median(mesh.values()) if mesh else 0
    late = {node for node, us in mesh.items() if us >= median + LATE_US}

    capture = Capture(events)
    out = ["%s: %d events, %s" % (directory, len(events), footer[-1] if footer else "no footer")]
    out.extend(capture.arp_totals())
    pairs = sorted({(key[1], key[2]) for key in capture.arp_sent if key[0] == "request"})
    twice = sum(1 for asker, target in pairs if len(capture.arp_sent[("request", asker, target)]) > 1)
    slow_pairs = []
    resolved_at = {}
    for asker, target in pairs:
        requests, resolved, how = capture.resolution(asker, target)
        resolved_at[(asker, target)] = resolved
        if resolved is None or resolved - requests[0] >= SLOW_NS:
            slow_pairs.append((asker, target))
    out.append("arp: %d pairs asked, %d asked more than once, %d took %s ms or more to be resolved or never were" % (
        len(pairs), twice, len(slow_pairs), ms(SLOW_NS)))

    records = connection_records(capture)
    syn = sum(len(r["syn"]) for r in records.values())
    out.append("tcp: SYN %d sent, %d received; SYN-ACK %d sent, %d received; %d RST frames" % (
        syn, sum(len(r["syn_received"]) for r in records.values()),
        sum(len(r["synack"]) for r in records.values()),
        sum(len(r["synack_received"]) for r in records.values()), capture.resets))
    slow = []
    retransmitted = 0
    refused = 0
    for key, record in sorted(records.items()):
        if not record["syn"]:
            continue
        if len(record["syn"]) > 1:
            retransmitted += 1
        if not record["synack_received"]:
            if key in capture.refused:
                refused += 1
            else:
                slow.append((key, None))
        elif record["synack_received"][0] - record["syn"][0] >= SLOW_NS:
            slow.append((key, record["synack_received"][0] - record["syn"][0]))
    out.append("tcp: %d connections were refused with an RST; %d sent their SYN more than once (same port, "
               "same sequence number); %d took %s ms or more from the first SYN to the SYN-ACK, or got none" % (
                   refused, retransmitted, len(slow), ms(SLOW_NS)))

    # The SYNs that waited for the ARP resolution of a slow pair: sent just after it ended.
    held_by_arp = []
    for key, record in sorted(records.items()):
        pair = (key[0][0], key[1][0])
        if pair in slow_pairs and record["syn"] and resolved_at.get(pair) is not None:
            if 0 <= record["syn"][0] - resolved_at[pair] <= 20000000:
                held_by_arp.append(key)
    out.append("tcp: %d connections sent their first SYN within 20 ms of a slow ARP resolution ending" %
               len(held_by_arp))

    # A dial made again from a new port: a connection no SYN-ACK answered and that was not refused,
    # and, from the same caller to the same answerer's port, another that began 0.9 to 1.5 s after it.
    redials = []
    by_pair = collections.defaultdict(list)
    for key, record in records.items():
        if record["syn"]:
            by_pair[(key[0][0], key[1])].append((record["syn"][0], key, record))
    for (caller, answerer), tries in by_pair.items():
        tries.sort(key=lambda item: item[0])
        for first, first_key, first_record in tries:
            if first_key in capture.refused or (first_record["synack_received"] and
                                                 first_key not in capture.dropped_by_caller):
                continue
            for second, second_key, _ in tries:
                if 0.9e9 <= second - first <= 1.5e9:
                    redials.append((caller, answerer, first, first_key[0][1], second_key[0][1], second - first))
                    break
    out.append("tcp: %d dials made again from a new port 0.9 to 1.5 s after one no SYN-ACK answered" %
               len(redials))

    causes = collections.Counter()
    explained = collections.defaultdict(list)
    stories = []
    for key, delay in slow:
        record = records[key]
        cause = capture.connection_cause(record)
        causes["tcp: " + cause] += 1
        caller, answerer = key[0][0], key[1][0]
        for node in (caller, answerer):
            explained[node].append("%d->%d (%s)" % (caller, answerer, cause))
        answered = ("answered after %s ms%s" % (ms(delay), ", too late: the caller had closed it and sent an RST"
                                                if key in capture.dropped_by_caller else "")
                    ) if delay is not None else "never answered"
        stories.append(["connection %d:%d -> %d:%d, %s: %s" % (
            key[0][0], key[0][1], key[1][0], key[1][1], answered, cause),
            "    syn sent at %s" % times(record["syn"], pair_up(record["syn"], record["syn_received"])),
            "    syn-ack sent at %s" % times(record["synack"],
                                            pair_up(record["synack"], record["synack_received"]))])
    for asker, target in slow_pairs:
        cause, story = capture.arp_story(asker, target)
        causes["arp: " + cause] += 1
        for node in (asker, target):
            explained[node].append("arp %d->%d (%s)" % (asker, target, cause))
        stories.append(story)
    for story in stories[:STORIES]:
        out.extend(story)
    if len(stories) > STORIES:
        out.append("... %d more stories" % (len(stories) - STORIES))
    for caller, answerer, first, first_port, second_port, gap in redials[:20]:
        out.append("redial: %d -> %d:%d from port %d at %s ms, again from port %d %s ms later" % (
            caller, answerer[0], answerer[1], first_port, ms(first), second_port, ms(gap)))
    out.append("causes: " + ("; ".join("%d x %s" % (n, c) for c, n in causes.most_common()) or "none"))
    if mesh:
        out.append("late leads: %s" % (" ".join(str(n) for n in sorted(late)) or "none"))
        for node in sorted(late):
            out.append("  lead %d: %s" % (node, ("%d slow: %s" % (len(explained[node]), "; ".join(
                explained[node][:4]))) if explained[node] else "no slow connection or ARP pair of its own seen"))
        accounted = sum(1 for node in late if explained[node])
        out.append("late leads accounted for by a slow connection or ARP pair: %d of %d" % (accounted, len(late)))
    requests, replies = capture.arp_counts("request"), capture.arp_counts("reply")
    syn_lost = sum(sum(1 for r in pair_up(record["syn"], record["syn_received"]) if r is None)
                   for record in records.values())
    synack_lost = sum(sum(1 for r in pair_up(record["synack"], record["synack_received"]) if r is None)
                      for record in records.values())
    dropped = re.search(r"dropped (\d+)", footer[-1]) if footer else None
    figures = {
        "events": len(events), "capture_dropped": int(dropped.group(1)) if dropped else -1,
        "arp_requests": requests[0], "arp_requests_lost": requests[2],
        "arp_replies": replies[0], "arp_replies_lost": replies[2], "arp_slow_pairs": len(slow_pairs),
        "syn": syn, "syn_lost": syn_lost, "synack": sum(len(r["synack"]) for r in records.values()),
        "synack_lost": synack_lost, "refused": refused, "same_port_resends": retransmitted,
        "slow_connections": len(slow), "redials": len(redials),
        "late_leads": len(late), "late_accounted": sum(1 for node in late if explained[node]),
    }
    return out, figures


def main():
    arguments = sys.argv[1:]
    totals = arguments[:1] == ["--totals"]
    if totals:
        arguments = arguments[1:]
    sums = collections.Counter()
    names = None
    width = max([len(directory.rstrip("/")) for directory in arguments] + [3])
    width = min(width, 40)
    for directory in arguments:
        lines, figures = analyse(directory)
        if not totals:
            for line in lines:
                print(line)
            print()
            continue
        if names is None:
            names = list(figures)
            print("%-*s " % (width, "run") + " ".join(names))
        label = "/".join(directory.rstrip("/").split("/")[-2:])
        print("%-*s " % (width, label) + " ".join("%*d" % (len(name), figures.get(name, 0)) for name in names))
        sums.update(figures)
    if totals and names:
        print("%-*s " % (width, "total") + " ".join("%*d" % (len(name), sums[name]) for name in names))


if __name__ == "__main__":
    main()
