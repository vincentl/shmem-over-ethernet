#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""T46's capture: the ARP and the TCP handshake packets of every node of a tier 3 job.

    t46-sniff.py PREFIX NODES OUTPUT READY     (as root, in the background; SIGTERM ends it)
    t46-sniff.py --selftest                    (any host: the filter against made-up frames)

For each node namespace PREFIX<i> one AF_PACKET socket on link0 keeps, by a
filter in the kernel,

  - every ARP frame the node sends, and every ARP frame addressed to it (a
    request that asks for its address, or the reply to its own request) - the
    flood of everyone else's requests is left in the kernel;
  - every TCP frame with the SYN or the RST bit set, sent or received.

A frame is seen where link0 sends it (after netem's delay, before the veth
pair's hand-over to the bridge) and where link0 receives it (after the
per-CPU backlog queue). A frame sent and never received was therefore lost
between the two, in the veth pairs and the bridge; one netem's loss dropped is
never sent at all. Each line of OUTPUT:

    <ns since the capture began> <node> <out|in> arp <request|reply> <sender node> <target node>
    <ns since the capture began> <node> <out|in> tcp <flags> <source node>:<port> <destination node>:<port> <seq> <ack>

A node is the last byte of its address, less one. The last lines, "# socket",
give each socket's frames kept by the filter and the frames the socket's own
buffer dropped (which must be none for the capture to be whole).
"""

import ctypes
import os
import select
import signal
import socket
import struct
import sys

ETHERNET_ALL = 3
SOL_PACKET = 263
PACKET_STATISTICS = 6
SO_ATTACH_FILTER = 26
SO_RCVBUFFORCE = 33
SO_TIMESTAMPNS = 35
PACKET_OUTGOING = 4
SNAPSHOT_LENGTH = 128
BUFFER_BYTES = 16 << 20

# Classic BPF, the instruction classes this filter uses.
LOAD_HALF_ABSOLUTE, LOAD_BYTE_ABSOLUTE, LOAD_WORD_ABSOLUTE = 0x28, 0x30, 0x20
LOAD_BYTE_INDEXED, LOAD_INDEX_HEADER_LENGTH = 0x50, 0xB1
JUMP_EQUAL, JUMP_SET, RETURN = 0x15, 0x45, 0x06
ANCILLARY_PACKET_TYPE = 0xFFFFF000 + 4          # SKF_AD_OFF + SKF_AD_PKTTYPE


def filter_program(node):
    """The kernel filter for node's link0, as (code, jump if true, jump if false, k) tuples."""
    address = 0x0A630000 + node + 1             # 10.99.0.<node + 1>
    keep, drop = 14, 15
    program = [
        (LOAD_HALF_ABSOLUTE, 0, 0, 12),                     #  0 ethertype
        (JUMP_EQUAL, 1, 0, 0x0806),                         #  1 ARP: to 3
        (JUMP_EQUAL, 4, 12, 0x0800),                        #  2 IPv4: to 7, else drop
        (LOAD_WORD_ABSOLUTE, 0, 0, ANCILLARY_PACKET_TYPE),  #  3 ARP: sent by this node?
        (JUMP_EQUAL, keep - 5, 0, PACKET_OUTGOING),         #  4
        (LOAD_WORD_ABSOLUTE, 0, 0, 38),                     #  5 the target protocol address
        (JUMP_EQUAL, keep - 7, drop - 7, address),          #  6 this node's: keep
        (LOAD_BYTE_ABSOLUTE, 0, 0, 23),                     #  7 IP protocol
        (JUMP_EQUAL, 0, drop - 9, 6),                       #  8 TCP: to 9
        (LOAD_HALF_ABSOLUTE, 0, 0, 20),                     #  9 flags and fragment offset
        (JUMP_SET, drop - 11, 0, 0x1FFF),                   # 10 a later fragment: drop
        (LOAD_INDEX_HEADER_LENGTH, 0, 0, 14),               # 11 X = the IP header's length
        (LOAD_BYTE_INDEXED, 0, 0, 27),                      # 12 TCP flags: 14 + X + 13
        (JUMP_SET, keep - 14, drop - 14, 0x06),             # 13 SYN or RST: keep
        (RETURN, 0, 0, SNAPSHOT_LENGTH),                    # 14 keep
        (RETURN, 0, 0, 0),                                  # 15 drop
    ]
    return program


def run_filter(program, frame, packet_type):
    """The kernel's verdict on frame, by the classic BPF rules: the snapshot length kept, 0 to drop."""
    accumulator = index = counter = 0

    def byte(offset):
        return frame[offset] if 0 <= offset < len(frame) else None

    while counter < len(program):
        code, jump_true, jump_false, k = program[counter]
        counter += 1
        if code == LOAD_HALF_ABSOLUTE:
            if k + 2 > len(frame):
                return 0
            accumulator = struct.unpack_from("!H", frame, k)[0]
        elif code == LOAD_BYTE_ABSOLUTE:
            if byte(k) is None:
                return 0
            accumulator = frame[k]
        elif code == LOAD_WORD_ABSOLUTE:
            if k == ANCILLARY_PACKET_TYPE:
                accumulator = packet_type
            else:
                if k + 4 > len(frame):
                    return 0
                accumulator = struct.unpack_from("!I", frame, k)[0]
        elif code == LOAD_INDEX_HEADER_LENGTH:
            if byte(k) is None:
                return 0
            index = (frame[k] & 0xF) << 2
        elif code == LOAD_BYTE_INDEXED:
            if byte(index + k) is None:
                return 0
            accumulator = frame[index + k]
        elif code == JUMP_EQUAL:
            counter += jump_true if accumulator == k else jump_false
        elif code == JUMP_SET:
            counter += jump_true if accumulator & k else jump_false
        elif code == RETURN:
            return k
        else:
            raise ValueError("instruction %#x" % code)
    return 0


def attach(sock, program):
    raw = b"".join(struct.pack("HBBI", *instruction) for instruction in program)
    buffer = ctypes.create_string_buffer(raw, len(raw))
    descriptor = struct.pack("@HL", len(program), ctypes.addressof(buffer))
    sock.setsockopt(socket.SOL_SOCKET, SO_ATTACH_FILTER, descriptor)


def node_of(address):
    """The node an address is, or None: 10.99.0.<k> is node k - 1."""
    if address[:3] == b"\x0a\x63\x00" and address[3] >= 1:
        return address[3] - 1
    return None


def describe(frame, packet_type):
    """One event's fields as text, or None for a frame the filter should not have kept."""
    (ethertype,) = struct.unpack_from("!H", frame, 12)
    direction = "out" if packet_type == PACKET_OUTGOING else "in"
    if ethertype == 0x0806:
        operation = struct.unpack_from("!H", frame, 20)[0]
        sender, target = node_of(frame[28:32]), node_of(frame[38:42])
        kind = {1: "request", 2: "reply"}.get(operation, "op%d" % operation)
        return direction, "arp %s %s %s" % (kind, sender, target)
    if ethertype == 0x0800 and frame[23] == 6:
        header = 14 + (frame[14] & 0xF) * 4
        source, destination = node_of(frame[26:30]), node_of(frame[30:34])
        source_port, destination_port, sequence, acknowledgement = struct.unpack_from(
            "!HHII", frame, header)
        bits = frame[header + 13]
        flags = "".join(letter for mask, letter in
                        ((0x02, "S"), (0x01, "F"), (0x04, "R"), (0x08, "P"), (0x10, "A"), (0x20, "U"))
                        if bits & mask)
        return direction, "tcp %s %s:%d %s:%d %d %d" % (
            flags, source, source_port, destination, destination_port, sequence, acknowledgement)
    return None


def selftest():
    def address(node):
        return bytes([10, 99, 0, node + 1])

    def ethernet(ethertype, payload):
        return b"\xff" * 6 + b"\x02\x00\x00\x00\x00\x01" + struct.pack("!H", ethertype) + payload

    def arp(operation, sender, target):
        return ethernet(0x0806, struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, operation,
                                            b"\x02" * 6, address(sender), b"\x00" * 6, address(target)))

    def tcp(bits, source, destination, header_words=5, fragment=0, protocol=6):
        options = b"\x01" * ((header_words - 5) * 4)
        ip = struct.pack("!BBHHHBBH4s4s", 0x40 | header_words, 0, 40, 1, fragment, 64, protocol, 0,
                         address(source), address(destination)) + options
        segment = struct.pack("!HHIIHHHH", 40000, 5000, 7, 0, (5 << 12) | bits, 1000, 0, 0)
        return ethernet(0x0800, ip + segment)

    me, other = 5, 9
    program = filter_program(me)
    cases = [
        ("ARP request sent by this node", arp(1, me, other), 4, True),
        ("ARP reply sent by this node", arp(2, me, other), 4, True),
        ("ARP request for this node", arp(1, other, me), 1, True),
        ("ARP reply to this node", arp(2, other, me), 0, True),
        ("ARP request for another node, flooded here", arp(1, 3, other), 1, False),
        ("ARP reply to another node", arp(2, 3, other), 3, False),
        ("SYN received", tcp(0x02, other, me), 0, True),
        ("SYN sent", tcp(0x02, me, other), 4, True),
        ("SYN-ACK", tcp(0x12, other, me), 0, True),
        ("RST", tcp(0x04, other, me), 0, True),
        ("ACK alone", tcp(0x10, other, me), 0, False),
        ("FIN-ACK", tcp(0x11, other, me), 0, False),
        ("SYN with IP options", tcp(0x02, other, me, header_words=7), 0, True),
        ("ACK with IP options", tcp(0x10, other, me, header_words=7), 0, False),
        ("a later fragment", tcp(0x02, other, me, fragment=0x00B9), 0, False),
        ("UDP", tcp(0x02, other, me, protocol=17), 0, False),
    ]
    failed = 0
    for name, frame, packet_type, expected in cases:
        kept = run_filter(program, frame, packet_type) > 0
        shown = describe(frame, packet_type) if kept else None
        print("%-46s %-4s %s" % (name, "kept" if kept else "drop", shown[1] if shown else ""))
        if kept != expected:
            print("  WRONG: expected %s" % ("kept" if expected else "dropped"))
            failed += 1
    sys.exit(1 if failed else 0)


def main():
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    prefix, nodes, output, ready = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    original = os.open("/proc/self/ns/net", os.O_RDONLY)
    sockets = []
    for node in range(nodes):
        descriptor = os.open("/run/netns/%s%d" % (prefix, node), os.O_RDONLY)
        os.setns(descriptor, os.CLONE_NEWNET)
        os.close(descriptor)
        # No protocol yet, so that nothing is received before the filter is in.
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, 0)
        attach(sock, filter_program(node))
        sock.setsockopt(socket.SOL_SOCKET, SO_RCVBUFFORCE, BUFFER_BYTES)
        sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
        sock.bind(("link0", ETHERNET_ALL))
        sock.setblocking(False)
        sockets.append(sock)
    os.setns(original, os.CLONE_NEWNET)
    os.close(original)

    stopping = []
    signal.signal(signal.SIGTERM, lambda number, frame: stopping.append(True))
    poller = select.epoll()
    owner = {}
    for node, sock in enumerate(sockets):
        poller.register(sock.fileno(), select.EPOLLIN)
        owner[sock.fileno()] = node
    events = []
    space = socket.CMSG_SPACE(16)

    def drain(sock, node):
        while True:
            try:
                frame, ancillary, _, address = sock.recvmsg(SNAPSHOT_LENGTH, space)
            except BlockingIOError:
                return
            stamp = None
            for level, kind, data in ancillary:
                if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS:
                    seconds, nanoseconds = struct.unpack("qq", data[:16])
                    stamp = seconds * 1000000000 + nanoseconds
            described = describe(frame, address[2])
            if described and stamp is not None:
                events.append((stamp, node, described[0], described[1]))

    with open(ready, "w") as handle:
        handle.write("ready\n")
    while not stopping:
        for descriptor, _ in poller.poll(0.2):
            drain(sockets[owner[descriptor]], owner[descriptor])
    for node, sock in enumerate(sockets):
        drain(sock, node)

    events.sort()
    base = events[0][0] if events else 0
    with open(output, "w") as handle:
        handle.write("# t46-sniff %s %d first_event_realtime_ns=%d\n" % (prefix, nodes, base))
        for stamp, node, direction, text in events:
            handle.write("%d %d %s %s\n" % (stamp - base, node, direction, text))
        total = dropped = 0
        for node, sock in enumerate(sockets):
            packets, drops = struct.unpack("II", sock.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8))
            total += packets
            dropped += drops
            handle.write("# socket %d kept %d dropped %d\n" % (node, packets, drops))
        handle.write("# total kept %d dropped %d\n" % (total, dropped))


if __name__ == "__main__":
    main()
