#!/usr/bin/env python3
"""Divide a thread's perf samples the way step 4's Part 0 asks.

usage: perf script -i perf.data -F comm,tid,period,ip,sym,dso | classify.py [--tid N]

Each sample is put in one bucket by its leaf frame and its stack:
  kernel copy     the socket copies: copy_to/from_user and the kernel's memcpy
  kernel network  TCP/IP, loopback, softirq - the stack around the copies
  io_uring        the ring's submit, issue, task work and reaping, in the kernel
  kernel other    scheduler, futex, timers, syscall entry
  library copy    libc's memcpy/memmove called from the library, split by caller
  library         libosoe's own code, split by the engine function it is under
  user other      liburing, libc but its copies, anything else
Prints each bucket's share of the samples, then the leading symbols in each.
"""
import collections
import re
import sys

KERNEL_COPY = re.compile(r'(__arch_copy_(to|from)_user|copy_(to|from)_user|_copy_(to|from)_iter|'
                         r'^memcpy$|^__memcpy|copy_page|csum_partial_copy)')
NETWORK = re.compile(r'^(tcp_|__tcp_|ip_|__ip_|ipv4_|__dev_queue_xmit|dev_hard_start_xmit|loopback_|'
                     r'net_rx_action|__netif_|netif_|napi_|__napi_|skb_|__skb_|sock_|__sock_|'
                     r'inet_|sk_|__sk_|do_softirq|__do_softirq|handle_softirqs|__local_bh_enable|'
                     r'release_sock|lock_sock|__release_sock|validate_xmit|dst_|nf_|__kfree_skb|'
                     r'kfree_skb|consume_skb|alloc_skb|__alloc_skb|tcp|raw_)')
RING = re.compile(r'^(io_|__io_|io_uring|tctx_|task_work_run)')
ENGINE = ['osoe_wire_recv', 'osoe_am_parse', 'receive', 'flush', 'drain', 'drain_into', 'finalize',
          'osoe_progress_poll', 'osoe_progress_host_poll', 'osoe_ring_wait_wake', 'osoe_wire_send',
          'osoe_internode_am_take_arrivals', 'osoe_deadlock_tick', 'agent_main']


def samples(stream):
    head, frames = None, []
    for line in stream:
        line = line.rstrip('\n')
        if not line.strip():
            if head:
                yield head, frames
            head, frames = None, []
        elif line[0] not in ' \t':
            if head:
                yield head, frames
            head, frames = line, []
        else:
            m = re.match(r'\s*([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$', line)
            if m:
                sym = re.sub(r'\+0x[0-9a-f]+$', '', m.group(2))
                frames.append((sym, m.group(3)))
    if head:
        yield head, frames


def classify(frames):
    if not frames:
        return 'unknown', '?'
    leaf, dso = frames[0]
    kernel = 'kernel' in dso
    if kernel:
        syms = [s for s, d in frames if 'kernel' in d]
        if KERNEL_COPY.search(leaf):
            return 'kernel copy', leaf
        if any(NETWORK.search(s) for s in syms):
            return 'kernel network', leaf
        if any(RING.search(s) for s in syms):
            return 'io_uring', leaf
        return 'kernel other', leaf
    if re.match(r'^(__)?mem(cpy|move)', leaf) or 'memcpy' in leaf or 'memmove' in leaf:
        caller = next((s for s, d in frames[1:] if 'libosoe' in d), None)
        return 'library copy', f'{leaf} <- {caller}'
    if 'libosoe' in dso:
        under = next((s for s, d in frames if s in ENGINE), leaf)
        return 'library', f'{leaf} (under {under})'
    return 'user other', f'{leaf} [{dso.split("/")[-1]}]'


def main():
    tid = None
    if '--tid' in sys.argv:
        tid = sys.argv[sys.argv.index('--tid') + 1]
    buckets = collections.Counter()
    detail = collections.defaultdict(collections.Counter)
    total = 0
    for head, frames in samples(sys.stdin):
        fields = head.split()
        if tid and tid not in fields:
            continue
        weight = 1                      # cpu-clock at a fixed rate: a sample is a tick
        bucket, what = classify(frames)
        buckets[bucket] += weight
        detail[bucket][what] += weight
        total += weight
    if not total:
        print('no samples')
        return
    for bucket, w in buckets.most_common():
        print(f'{bucket:16s} {100.0 * w / total:5.1f}%')
    print()
    for bucket, _ in buckets.most_common():
        print(f'== {bucket}')
        for what, w in detail[bucket].most_common(8):
            print(f'   {100.0 * w / total:5.1f}%  {what}')


if __name__ == '__main__':
    main()
# SPDX-License-Identifier: LGPL-3.0-or-later
