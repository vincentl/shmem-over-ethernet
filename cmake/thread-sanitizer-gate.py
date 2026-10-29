#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""ThreadSanitizer's gate (OsoeThreadSanitizer.cmake; the ninth review's C9-1).

  thread-sanitizer-gate.py setup REPORTS CANARY
      empties REPORTS, runs the canary - whose one race the tool must report
      there - and fails if the report is not there.
  thread-sanitizer-gate.py judge REPORTS --ctest CTEST --build BUILD --place PLACE
      judges every report file in REPORTS, each named for the test whose
      process wrote it (log_path=REPORTS/<test>, the tool adding .<pid>).

What passes is one kind of report only, the README's "How osoe is tested"
says which: a data race in a PE's heap between a progress helper's landing and
a local thread's access - or another helper's landing - ordered through another
process, where the tool cannot see the ordering. Each must:

- be a data race. A report of any other kind fails - a thread leak, a lock
  inversion, a signal handler's unsafe call - and so does every ERROR, the
  tool's word for a crash it caught, and every FATAL or CHECK failed, its word
  for its own failures, whatever else the file holds;
- have its location in a heap object (osoe.<job>.<step>.<node>.<session>.heap.<pe>)
  and every access in the heaps' range, from OSOE_HEAP_BASE, 0x010000000000
  under the tool unless the test sets it;
- name a progress helper's thread, osoe-helper-N: with no helper the one agent
  applies every landing, and the ordering is the tool's to see (round eight's
  Q2-2);
- lie in the program's heap, below the test's page-rounded SHMEM_SYMMETRIC_SIZE,
  read from ctest's own list of the tests; or else, in the library's internal
  region at the heap object's tail (internal-region.h), be one of INTERNAL's,
  by its place - PLACE, built from the library's layout, names it - and the
  functions of its two accesses, each with the reason it is ordered after all
  (round eight's Q2-2): there a helper's real race would otherwise hide.

The canary's report must be there, and is not judged. Exit 0 when every report
passes.
"""
import json
import os
import re
import shutil
import subprocess
import sys

HEAP_BASE = 0x010000000000
HEAP_SPAN = 1 << 40
DEFAULT_SYMMETRIC_SIZE = 256 << 20       # OSOE_DEFAULT_SYMMETRIC_SIZE, bootstrap.h

# The races the gate passes in the internal region: the place (None for any of
# the team number's words), the function of the helper's landing, the function
# of the other access - another helper's landing, or a local thread's - and why
# the two are ordered, through other nodes.
INTERNAL = [
    (None, 'osoe_am_parse', 'osoe_internode_generic_number_taken',
     'a landing in a team number\'s words against their zeroing when a split '
     'took the number (team.c, finish): every member zeroes its words before '
     'the split\'s sync, and no collective on the new team starts before every '
     'member has left it'),
    ('grant-words', 'osoe_am_parse', 'osoe_am_parse',
     'a parent\'s grant words, put once each large collective, against the '
     'last collective\'s - another parent\'s where the tree is rotated to '
     'another root: the next is put only once the collective before has ended, '
     'by which this lead had taken and cleared them (take_grant)'),
    ('grant-words', 'osoe_am_parse', 'take_grant',
     'a parent\'s grant words against this lead\'s taking and clearing of the '
     'collective before\'s: the same order, the next put only once that '
     'collective has ended'),
    ('lane-word', 'osoe_am_parse', 'osoe_am_parse',
     'a child\'s lane word, put before its first arrival of each large '
     'collective, against the last collective\'s - another child\'s at the '
     'position where the tree is rotated to another root: the next is put only '
     'once the collective before has ended, by which this lead had taken it '
     '(take_child_lanes; lane_model.py, release_lane_late)'),
    ('lane-word', 'osoe_am_parse', 'take_child_lanes',
     'a child\'s lane word against this lead\'s taking and clearing of the '
     'collective before\'s: the same order'),
    ('small-slot', 'osoe_am_parse', 'osoe_am_parse',
     'a child\'s piece in this lead\'s small slot, put once each small '
     'collective, against the last collective\'s - another child\'s, landed by '
     'another helper since helpers run by default (P36): a reader clears what it '
     'read, and the next is put only after the reader\'s next arrival, which the '
     'clear precedes (internode-generic.c, cleared)'),
    ('small-slot', 'osoe_am_parse', 'cleared',
     'a child\'s piece in this lead\'s small slot against this lead\'s clearing '
     'of the last collective\'s once combined: the same order'),
    ('past-the-team-words', 'osoe_am_parse', 'written',
     'a landing in a lead\'s grant fields against test-circulant-grants.c\'s own '
     'look at them, between two barriers: the next reduction\'s pieces are put '
     'only once every member has left the barrier after the look'),
]

REPORT_START = re.compile(r'^WARNING: ThreadSanitizer: (.+?) \(pid=\d+\)', re.M)
ACCESS = re.compile(r'(?:[Ww]rite|[Rr]ead|[Aa]tomic \w+) of size \d+ at (0x[0-9a-f]+) by '
                    r'(?:main thread|thread (T\d+))')
THREAD = re.compile(r"Thread (T\d+) '([^']*)'")
FRAME = re.compile(r'^\s+#\d+ (\S+) ', re.M)
SETTINGS = {
    'size': re.compile(r'(?:SHMEM|SMA)_SYMMETRIC_SIZE=([0-9]+[kKmMgGtT]?)\b'),
    'base': re.compile(r'OSOE_HEAP_BASE=(0x[0-9a-fA-F]+|[0-9]+)\b'),
    'fanout': re.compile(r'OSOE_COLLECTIVE_FANOUT=([0-9]+)\b'),
    'teams': re.compile(r'OSOE_MAX_TEAMS=([0-9]+)\b'),
    'small': re.compile(r'OSOE_SMALL_COLLECTIVE_BYTES=([0-9]+)\b'),
}
DEFAULTS = {'size': DEFAULT_SYMMETRIC_SIZE, 'base': HEAP_BASE, 'fanout': 0, 'teams': 0, 'small': 0}


def parse_number(name, text):
    if name == 'size':
        multiplier = {'k': 1 << 10, 'm': 1 << 20, 'g': 1 << 30, 't': 1 << 40}
        if text[-1].lower() in multiplier:
            return int(text[:-1]) * multiplier[text[-1].lower()]
    return int(text, 0)


def setup(reports, canary):
    shutil.rmtree(reports, ignore_errors=True)
    os.makedirs(reports)
    run = subprocess.run([canary], capture_output=True, text=True)
    files = [f for f in os.listdir(reports) if f.startswith('tsan.reports.setup.')]
    if run.returncode != 0 or not files:
        print(f'the canary exited {run.returncode} and left {len(files)} report files in '
              f'{reports}: the tool\'s options did not reach it\n{run.stdout}{run.stderr}')
        return 1
    print(f'the canary\'s report is in {reports}/{files[0]}')
    return 0


def test_settings(ctest, build):
    """Each test's heap and collective settings, from ctest's list of the tests:
    its command - `cmake -E env` and osoe-launch's -e set them there - its
    ENVIRONMENT, and the text of any shell script the command runs, which may
    set them itself (link.sh sets a fan-out of 2: the read of these changes,
    R9F-3). A setting found with two values is None, which the judging refuses
    rather than guess."""
    listing = subprocess.run([ctest, '--test-dir', build, '--show-only=json-v1'],
                             capture_output=True, text=True, check=True)
    settings = {}
    for test in json.loads(listing.stdout)['tests']:
        texts = list(test.get('command', []))
        for word in test.get('command', []):
            if word.endswith('.sh') and os.path.isfile(word):
                texts.append(open(word, errors='replace').read())
        for p in test.get('properties', []):
            if p['name'] in ('ENVIRONMENT', 'ENVIRONMENT_MODIFICATION'):
                texts += p['value'] if isinstance(p['value'], list) else [p['value']]
        joined = '\n'.join(texts)
        mine = {}
        for name, pattern in SETTINGS.items():
            values = {parse_number(name, v) for v in pattern.findall(joined)}
            mine[name] = DEFAULTS[name] if not values else (values.pop() if len(values) == 1 else None)
        settings[test['name']] = mine
    return settings


def accesses_of(block):
    """The report's accesses: each one's address, the name of the thread that
    made it, and its stack's function names."""
    threads = dict(THREAD.findall(block))
    out = []
    parts = re.split(r'\n(?=  (?:Previous )?(?:[Ww]rite|[Rr]ead|[Aa]tomic \w+) of size)', block)
    for part in parts:
        m = ACCESS.search(part)
        if not m:
            continue
        name = threads.get(m.group(2), m.group(2)) if m.group(2) else 'main thread'
        out.append((int(m.group(1), 16), name, FRAME.findall(part.split('\n\n')[0])))
    return out


def judge_block(kind, block, settings, place_of, why):
    """The report's problems, none if it passes; `why` counts the reasons the
    passed ones pass by."""
    if kind != 'data race':
        return [f'a report of another kind: {kind}']
    problems = []
    locations = re.findall(r'Location is ([^\n]*)', block)
    if not locations:
        problems.append('a race with no location')
    for location in locations:
        if '.heap.' not in location:
            problems.append(f'a race outside the heaps: {location}')
    accesses = accesses_of(block)
    if len(accesses) < 2:
        problems.append('a race with fewer than two accesses shown')
    helpers = [a for a in accesses if a[1].startswith('osoe-helper-')]
    if not helpers:
        problems.append('a race no progress helper made, between '
                        + ' and '.join(sorted({a[1] for a in accesses})))
    if None in settings.values():
        problems.append('the test\'s heap or collective settings have two values in '
                        'ctest\'s list of it')
    if problems:
        return problems
    base = settings['base']
    page = os.sysconf('SC_PAGE_SIZE')
    program = (settings['size'] + page - 1) // page * page
    inside = []
    for address, _, _ in accesses:
        if not base <= address < base + HEAP_SPAN:
            problems.append(f'an access outside the heaps\' range: {address:#x}')
        elif address - base >= program:
            inside.append(address - base - program)
    if problems:
        return problems
    if not inside:
        why['in a program\'s heap'] = why.get('in a program\'s heap', 0) + 1
        return problems
    places = place_of(settings, inside)
    landing = helpers[0][2]
    others = [a[2] for a in accesses if a is not helpers[0]]
    for where in set(places):
        matched = [(p, land, against) for p, land, against, _ in INTERNAL
                   if (p is None or p == where) and land in landing and
                   any(against in frames for frames in others)]
        if not matched:
            problems.append(f'a race in the library\'s internal region, at its {where}, not '
                            f'among those explained: {"/".join(landing[:3])} against '
                            f'{"/".join(others[0][:3])}')
        else:
            p, land, against = matched[0]
            key = f'in the internal region, at its {where}: {land} against {against}'
            why[key] = why.get(key, 0) + 1
    return problems


def judge(reports, ctest, build, place):
    files = sorted(os.listdir(reports))
    canary = [f for f in files if f.startswith('tsan.reports.setup.')]
    if not canary:
        print(f'the canary\'s report is missing from {reports}: '
              'the tool\'s options did not reach the tests')
        return 1
    settings = test_settings(ctest, build)

    def place_of(mine, offsets):
        run = subprocess.run([place, str(mine['fanout']), str(mine['teams']), str(mine['small'])]
                             + [f'{o:#x}' for o in offsets], capture_output=True, text=True,
                             check=True)
        return run.stdout.split()

    bad = reports_read = 0
    passed, why = {}, {}
    for f in files:
        if f in canary:
            continue
        m = re.match(r'(.+)\.(\d+)$', f)
        test = m.group(1) if m else f
        text = open(os.path.join(reports, f), errors='replace').read()
        # A crash the tool caught (ERROR), and the tool's own failures (FATAL,
        # CHECK failed), fail the file whatever else it holds (the read of
        # these changes, R9F-4).
        problems = [f'the tool says: {m.group(0).strip()}' for m in re.finditer(
            r'^.*(?:ERROR: ThreadSanitizer|FATAL: ThreadSanitizer|ThreadSanitizer: CHECK failed).*$',
            text, re.M)]
        starts = list(REPORT_START.finditer(text))
        mine = settings.get(test)
        for i, start in enumerate(starts):
            end = starts[i + 1].start() if i + 1 < len(starts) else len(text)
            reports_read += 1
            if mine is None:
                block_problems = ['a report of a test not in ctest\'s list']
            else:
                block_problems = judge_block(start.group(1), text[start.start():end], mine,
                                             place_of, why)
            problems += block_problems
            if not block_problems:
                passed[test] = passed.get(test, 0) + 1
        if not starts and not problems:
            problems.append('a report file with no report in it')
        for p in problems:
            print(f'{test} ({f}): {p}')
        bad += bool(problems)
    print(f'{len(files) - len(canary)} report files, {reports_read} reports, '
          f'{sum(passed.values())} passed in {len(passed)} tests')
    for reason in sorted(why):
        print(f'  passed {why[reason]}: {reason}')
    for test in sorted(passed):
        print(f'  passed: {test}: {passed[test]}')
    print('THREAD SANITIZER GATE PASSED' if bad == 0 else
          f'THREAD SANITIZER GATE FAILED: {bad} report files')
    return 1 if bad else 0


def main(argv):
    if len(argv) == 3 and argv[0] == 'setup':
        return setup(argv[1], argv[2])
    if (len(argv) == 8 and argv[0] == 'judge' and argv[2] == '--ctest' and
            argv[4] == '--build' and argv[6] == '--place'):
        return judge(argv[1], argv[3], argv[5], argv[7])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
