#!/usr/bin/env python3
"""v0.2 step 4 part 3: the gate on ThreadSanitizer's reports at a helper count,
test by test (the second adversarial read of Part 3, S2: the first gate read
the reports alone, and passed a run in which a test had failed by a timeout).

Each failing test of a ctest log run with --output-on-failure must have failed
by ThreadSanitizer alone: at least one report; every report a data race in a
PE's heap - its location a heap object (osoe.<job>.<step>.<node>.<session>.heap.<pe>),
and every access the reports show in the heaps' range (0x0100...), judged over
the test's whole output, since its processes' reports interleave - which is a
landing a helper applies, against a local thread's access or another helper's
landing to the same place, ordered through another process (README, How osoe
is tested); and every rank that left nonzero left with TSan's status, 66,
or was ended by the launcher (signal 15) after one did. A test that timed out
fails the gate unless named with --timeout-explained, with its reason in the
record. Any other kind of report, a race elsewhere, or a failure of another
kind fails it.
usage: tsan-gate.py [--timeout-explained NAME,...] LOG... ; exit 0 when every
failing test passes."""
import re
import sys

args = sys.argv[1:]
explained = set()
if args[:1] == ['--timeout-explained']:
    explained = set(args[1].split(','))
    args = args[2:]

RESULT = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: (\S+) \.+\s*(\*\*\*\w+|Passed)')


def blocks(text):
    """Each failed test's name and the output ctest printed after its result."""
    name, lines = None, []
    for line in text.splitlines():
        m = RESULT.match(line)
        if m or line.startswith('The following tests FAILED') or \
                re.match(r'^\d+% tests passed', line):
            if name:
                yield name, '\n'.join(lines)
            name, lines = (m.group(1) if m and m.group(2) != 'Passed' else None), []
            continue
        if name:
            lines.append(line)
    if name:
        yield name, '\n'.join(lines)


bad = 0
for fn in args:
    text = open(fn, errors='replace').read()
    tests = 0
    for name, out in blocks(text):
        tests += 1
        problems = []
        kinds = re.findall(r'WARNING: ThreadSanitizer: ([a-z -]+?) \(', out)
        others = sorted(set(k for k in kinds if k != 'data race'))
        if others:
            problems.append(f'reports of another kind: {others}')
        if not kinds:
            problems.append('no ThreadSanitizer report')
        # The test's reports judged together, not one by one: its processes
        # report at once, and their lines interleave - a report's header can
        # land alone between two of another's separators (verify-14,
        # shmem.circulant.5.capacity-1) - so every location and every access
        # the block shows must be in the heaps, wherever it landed.
        for location in re.findall(r'Location is ([^\n]*)', out):
            if '.heap.' not in location:
                problems.append(f'a race outside the heaps: {location}')
        addresses = re.findall(r'(?:[Ww]rite|[Rr]ead|[Aa]tomic \w+) of size \d+ at (0x[0-9a-f]+)', out)
        outside = sorted(set(a for a in addresses if not a.startswith('0x0100')))
        if outside:
            problems.append(f'accesses outside the heaps\' range: {outside}')
        if kinds and not addresses:
            problems.append('reports with no access shown')
        statuses = re.findall(r'osoe-launch: rank \d+ exited with status (\d+)', out)
        signals = re.findall(r'osoe-launch: rank \d+ killed by signal (\d+)', out)
        timed_out = 'osoe-launch: timeout after' in out
        if any(s != '66' for s in statuses):
            problems.append(f'exit statuses other than 66: {sorted(set(statuses))}')
        if not timed_out and (any(s != '15' for s in signals) or (signals and not statuses)):
            problems.append(f'ranks killed by signals {sorted(set(signals))} without a report\'s exit')
        if timed_out:
            if name in explained:
                print(f'{fn}: {name}: timed out, explained (--timeout-explained)')
            else:
                problems.append('timed out')
        for p in problems:
            print(f'{fn}: {name}: {p}')
        bad += bool(problems)
    print(f'{fn}: {tests} failing tests read')
print('TSAN GATE PASSED' if bad == 0 else f'TSAN GATE FAILED: {bad} tests')
sys.exit(1 if bad else 0)
# SPDX-License-Identifier: LGPL-3.0-or-later
