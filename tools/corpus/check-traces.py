#!/usr/bin/env python3
"""Check virtual traces against their format and the pinned corpus.

For every docs/design/workloads/traces/*.trace (or the files named), checks:
  - the header is present and in order, and the id matches the file name;
  - the source names a manifest entry at the same pin;
  - blocks balance, and every step has valid fields;
  - every cited file exists in the corpus and every cited line is in range;
  - each call's routine is spelled on its cited line. When the call is built
    by a macro, the step's note may say "spelled at :N" for the line in the
    same file that names it.

The corpus is read from corpus/ at the root of the main checkout, as
tools/corpus/fetch.sh places it, or from $OSOE_CORPUS. Fetch it first.
Needs only Python 3 and git.

Usage: tools/corpus/check-traces.py [file.trace...]
Exits 0 when every trace passes, 1 otherwise.
"""
import glob
import os
import re
import subprocess
import sys

HEADER = ["trace", "workload", "source", "loop", "threads"]
OPTIONAL_HEADER = ["build", "param"]
BLOCKS = ("repeat", "when", "role")
TARGETS = ("self", "neighbour", "random", "each", "all", "-")
CONTEXTS = ("default", "thread", "shared", "-")
CITE = re.compile(r"^(?P<path>[^:]+):(?P<first>\d+)(?:-(?P<last>\d+))?$")

here = os.path.dirname(os.path.abspath(__file__))
tree = subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                      check=True, capture_output=True, text=True).stdout.strip()
common = subprocess.run(["git", "-C", here, "rev-parse", "--path-format=absolute",
                         "--git-common-dir"],
                        check=True, capture_output=True, text=True).stdout.strip()
corpus = os.environ.get("OSOE_CORPUS") or os.path.join(os.path.dirname(common), "corpus")

failures = 0


def fail(where, message):
    global failures
    failures += 1
    print(f"{where}: {message}")


def read_manifest():
    pins = {}
    with open(os.path.join(here, "manifest.tsv")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) == 5:
                pins[fields[0]] = fields[3]
    return pins


_lines = {}


def source_lines(path):
    if path not in _lines:
        with open(path, errors="replace") as f:
            _lines[path] = f.read().split("\n")
    return _lines[path]


def check_cite(where, source, cite):
    """Returns the cited file's lines and the first cited line, or None."""
    m = CITE.match(cite)
    if not m:
        fail(where, f"citation '{cite}' is not path:line or path:first-last")
        return None
    path = os.path.join(corpus, source, m["path"])
    if not os.path.isfile(path):
        fail(where, f"no file {source}/{m['path']} in the corpus")
        return None
    lines = source_lines(path)
    first, last = int(m["first"]), int(m["last"] or m["first"])
    if not 1 <= first <= last <= len(lines):
        fail(where, f"lines {first}-{last} outside {m['path']} ({len(lines)} lines)")
        return None
    return lines, first


def check(file, pins):
    name = os.path.basename(file)
    header = []
    source = None
    depth = 0
    in_body = False
    with open(file) as f:
        for number, raw in enumerate(f, 1):
            where = f"{name}:{number}"
            text, _, note = raw.partition("#")
            fields = text.split()
            if not fields:
                continue
            key = fields[0]

            if key in HEADER + OPTIONAL_HEADER:
                if in_body:
                    fail(where, f"header line '{key}' after the body began")
                header.append(key)
                if len(fields) < 2:
                    fail(where, f"'{key}' needs a value")
                    continue
                if key == "trace" and fields[1] + ".trace" != name:
                    fail(where, f"trace id '{fields[1]}' does not match the file name")
                elif key == "source":
                    source = fields[1]
                    if len(fields) != 3 or pins.get(source) != fields[2]:
                        fail(where, "source is not a manifest entry at its pinned value")
                    elif not os.path.isdir(os.path.join(corpus, source)):
                        fail(where, f"{source} is not fetched into {corpus}")
                        source = None
                elif key == "loop" and source:
                    check_cite(where, source, fields[1])

            elif key in BLOCKS:
                in_body = True
                depth += 1
                if len(fields) < 2:
                    fail(where, f"'{key}' needs a count or description")

            elif key == "end":
                depth -= 1
                if depth < 0:
                    fail(where, "'end' without an open block")
                    depth = 0

            elif key in ("call", "direct"):
                in_body = True
                if len(fields) != 7:
                    fail(where, f"'{key}' needs 6 fields, has {len(fields) - 1}")
                    continue
                _, routine, elem, count, target, ctx, cite = fields
                if key == "call" and not re.match(r"^p?shmemx?_\w+$", routine):
                    fail(where, f"'{routine}' is not an OpenSHMEM routine name")
                if key == "direct" and routine not in ("load", "store"):
                    fail(where, "direct access must be 'load' or 'store'")
                if not re.match(r"^(\d+|-)$", elem):
                    fail(where, f"elem '{elem}' is not a byte count or '-'")
                if target not in TARGETS:
                    fail(where, f"target '{target}' is not one of {', '.join(TARGETS)}")
                if ctx not in CONTEXTS:
                    fail(where, f"ctx '{ctx}' is not one of {', '.join(CONTEXTS)}")
                if not source:
                    continue
                cited = check_cite(where, source, cite)
                if key != "call" or not cited:
                    continue
                lines, first = cited
                if re.search(rf"\b{routine}\b", lines[first - 1]):
                    continue
                pointers = [int(n) for n in re.findall(r"spelled at :(\d+)\b", note)]
                if not any(1 <= n <= len(lines) and re.search(rf"\b{routine}\b", lines[n - 1])
                           for n in pointers):
                    fail(where, f"{routine} is not on line {first}, "
                                "and no 'spelled at :N' in the note names a line that has it")

            else:
                fail(where, f"unknown line '{key}'")

    if header[:len(HEADER)] != HEADER:
        fail(name, f"header must begin {' '.join(HEADER)}")
    if depth:
        fail(name, f"{depth} block(s) not closed")


def main():
    pins = read_manifest()
    files = sys.argv[1:] or sorted(glob.glob(
        os.path.join(tree, "docs/design/workloads/traces/*.trace")))
    if not files:
        print("no traces found")
        return 1
    if not os.path.isdir(corpus):
        print(f"no corpus at {corpus}; run tools/corpus/fetch.sh first")
        return 1
    for file in files:
        check(file, pins)
    print(f"{len(files)} traces, {failures} problem(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
