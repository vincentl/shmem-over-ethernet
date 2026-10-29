# SPDX-License-Identifier: LGPL-3.0-or-later
"""H29's bare stream: TCP between the two tasks of an srun step, one way or both ways at once.

    srun -N2 --ntasks-per-node=1 python3 h29-tcp.py oneway|bothways SECONDS

Task 0 listens and task 1 connects to task 0's node. One way, task 0 sends and task 1
receives; both ways, each sends and receives at once on the one connection. Each task
prints what it sent and received a second, MiB/s, over SECONDS after a second's warm-up."""
import os
import socket
import subprocess
import sys
import threading
import time

PORT = 47290
CHUNK = 1 << 20

mode, seconds = sys.argv[1], float(sys.argv[2])
rank = int(os.environ['SLURM_PROCID'])
hosts = subprocess.run(['scontrol', 'show', 'hostnames', os.environ['SLURM_STEP_NODELIST']],
                       capture_output=True, text=True, check=True).stdout.split()
if rank == 0:
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('', PORT))
    listener.listen(1)
    connection, _ = listener.accept()
else:
    deadline = time.monotonic() + 30
    while True:
        try:
            connection = socket.create_connection((hosts[0], PORT))
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.1)
start = time.monotonic() + 1.0
end = start + seconds
counted = {'sent': 0, 'received': 0}


def send():
    block = b'\0' * CHUNK
    while True:
        now = time.monotonic()
        if now >= end:
            break
        connection.sendall(block)
        if now >= start:
            counted['sent'] += CHUNK
    connection.shutdown(socket.SHUT_WR)


def receive():
    buffer = bytearray(CHUNK)
    while True:
        n = connection.recv_into(buffer)
        if n == 0:
            break
        now = time.monotonic()
        if start <= now < end:
            counted['received'] += n


sends = mode == 'bothways' or rank == 0
receives = mode == 'bothways' or rank == 1
threads = []
if sends:
    threads.append(threading.Thread(target=send))
else:
    connection.shutdown(socket.SHUT_WR)
if receives:
    threads.append(threading.Thread(target=receive))
for t in threads:
    t.start()
for t in threads:
    t.join()
if not receives:
    while connection.recv(CHUNK):
        pass
print('tcp mode=%s rank=%d host=%s sent_mib_per_s=%.1f received_mib_per_s=%.1f'
      % (mode, rank, socket.gethostname(), counted['sent'] / seconds / (1 << 20),
         counted['received'] / seconds / (1 << 20)), flush=True)
