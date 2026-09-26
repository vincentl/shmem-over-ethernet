#!/usr/bin/env python3
"""tcp-bruck.py BLOCK [ITERATIONS]: osoe's four-node allgather traffic over
plain TCP, no osoe code. Rank r (SLURM_PROCID, on 10.0.1.r+1) keeps one
connection to each other rank; each iteration sends BLOCK bytes to r-1 while
receiving BLOCK from r+1, then 2*BLOCK to r-2 while receiving from r+2, then
a one-byte token around the ring as a barrier. Rank 0 prints the median, the
slowest, and how many iterations took over 20 ms."""
import os, socket, struct, sys, threading, time

block = int(sys.argv[1]); iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 400
overlap = len(sys.argv) > 3 and sys.argv[3] == 'overlap'   # both steps at once
r = int(os.environ['SLURM_PROCID']); n = int(os.environ['SLURM_NTASKS'])
nodes = os.environ.get('F1_NODES', ','.join(f'10.0.1.{i + 1}' for i in range(n))).split(',')
port = 47000
listener = socket.socket(); listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('0.0.0.0', port + r)); listener.listen(n)
peers = {}
def accept_lower():
    for _ in range(r):
        s, _ = listener.accept(); peer = struct.unpack('!i', s.recv(4))[0]; peers[peer] = s
t = threading.Thread(target=accept_lower); t.start()
for p in range(r + 1, n):
    while True:
        try:
            s = socket.create_connection((nodes[p], port + p)); break
        except OSError:
            time.sleep(0.1)
    s.sendall(struct.pack('!i', r)); peers[p] = s
t.join()
for s in peers.values():
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
def recv_exact(s, k):
    buf = bytearray(k); view = memoryview(buf); got = 0
    while got < k:
        m = s.recv_into(view[got:], k - got)
        if m == 0: raise SystemExit('closed')
        got += m
def exchange(to, frm, k):
    payload = b'x' * k
    th = threading.Thread(target=peers[to].sendall, args=(payload,)); th.start()
    recv_exact(peers[frm], k); th.join()
times = []
for i in range(iterations):
    t0 = time.monotonic()
    if overlap and n > 2:
        a = threading.Thread(target=exchange, args=((r - 1) % n, (r + 1) % n, block))
        a.start()
        exchange((r - 2) % n, (r + 2) % n, 2 * block)
        a.join()
    else:
        exchange((r - 1) % n, (r + 1) % n, block)
        if n > 2:
            exchange((r - 2) % n, (r + 2) % n, 2 * block)
    exchange((r + 1) % n, (r - 1) % n, 1)          # the barrier token
    times.append(time.monotonic() - t0)
times.sort()
if r == 0:
    print(f'tcp-bruck n {n} block {block} median_us {times[len(times)//2]*1e6:.1f} '
          f'max_ms {times[-1]*1e3:.1f} slow {sum(x > 0.02 for x in times)}', flush=True)
