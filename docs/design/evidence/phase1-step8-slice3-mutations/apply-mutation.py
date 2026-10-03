#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""apply-mutation.py NAME: apply mutate.sh's mutation NAME to shmem/src/transport-am.c
in the current directory; exits nonzero if its target is not there once."""
import sys
src = open('<repo>/build/scratch/slice3/mutate.sh').read()
block = src[src.index("edits = {"):src.index("}\nold, new") + 1]
ns = {}
exec(block, ns)
old, new = ns['edits'][sys.argv[1]]
p = 'shmem/src/transport-am.c'
s = open(p).read()
if s.count(old) != 1:
    sys.exit(f'{sys.argv[1]}: target found {s.count(old)} times')
open(p, 'w').write(s.replace(old, new))
