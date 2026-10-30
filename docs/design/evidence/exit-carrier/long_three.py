#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""abort_exit_model.py's `three` under P1's proposal - A's PA and B's PB and QB calling
shmem_global_exit at once, under srun -K1 - without the model's half-hour limit: the same
case and options, given four hours and the engine's own bound of states."""
import sys

import abort_exit_model as model
from engine import explore, report

name, threads, mem, processes = model.case('single', 'three', model.KILLS)
res = explore(name, threads, mem, done=lambda M: all(not M[model.ALIVE(p)] for p in processes),
              liveness=True, max_seconds=4 * 3600, liveness_max_states=12_000_000)
sys.exit(0 if report(res, expect=True, tso=False) else 1)
