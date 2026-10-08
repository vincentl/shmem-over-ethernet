#!/usr/bin/env python3
"""Apply one named mutation of the fill-the-room rung to a tree. usage: mutate.py TREE NAME"""
import pathlib, sys
tree, name = pathlib.Path(sys.argv[1]), sys.argv[2]
M = {
    # the header keeps the whole message's length when the part is cut
    'header_not_cut': ('shmem/src/transport-internode.c',
        "            body = (uint32_t)sizeof m + n;\n            h.len = body;\n",
        "            body = (uint32_t)sizeof m + n;\n"),
    # the part's payload length is not cut to the reservation
    'payload_not_cut': ('shmem/src/transport-internode.c',
        "            n = r.len - overhead;\n", ""),
    # the length is decided from one load of the head and the CAS from another
    'stale_head': ('shmem/src/connection.c',
        "        if (atomic_compare_exchange_weak_explicit(&s->buffer[k].head, &h, h + len,",
        "        h = atomic_load_explicit(&s->buffer[k].head, memory_order_seq_cst);\n"
        "        if (atomic_compare_exchange_weak_explicit(&s->buffer[k].head, &h, h + len,"),
    # the floor is not lowered to a remainder smaller than it
    'floor_whole': ('shmem/src/transport-internode.c',
        "(n < smallest_part ? n : smallest_part));", "smallest_part);"),
    # the remainder is cut by one message alignment too few
    'payload_short': ('shmem/src/transport-internode.c',
        "            n = r.len - overhead;\n", "            n = r.len - overhead - 8;\n"),
}
path, old, new = M[name]
p = tree / path
s = p.read_text()
assert s.count(old) == 1, (name, s.count(old))
p.write_text(s.replace(old, new))
print('mutated', name)
