#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# collect-places.sh BUILD OUTDIR [NODES...]
#
# H24 of docs/prompts/v0.2-open-items.md: collect's places timed by each way at
# each count of logical nodes, one PE a node - the members' vector, each node's
# total through the tree (P29), the leads' scan of the totals (P30), the leads'
# exchange of them (P33), and the default, the way the link makes cheapest (P31,
# collective-shape.c's osoe_places_choose) - each with osoe-bench's three
# patterns of lengths: every PE's 8 bytes, every PE's 0 but every sixteenth's,
# and PE 0's 64 KiB with every other's 8 (osoe-bench places quick: the median
# and fastest of 200 collects after 200 more). NODES are 8 16 32 64 128 254 by
# default, tier 3's logical nodes; past 64 the exchange falls back to each
# node's total (internal-region.h, its room), which the table marks.
#
# TIER3=1 runs every job over tier 3's namespaces and the Pi-like link
# (tools/launch/tier3.sh pi: sudo -n, and netem in the kernel), as H24 asks;
# otherwise over loopback, every node on this machine as tier 2 runs its maps,
# which is the sample a machine without netem can take - its numbers are that
# machine's, and four CPUs carrying 254 nodes' leads, agents and progress
# threads measure the CPUs as much as the ways. So does tier 3: its logical
# nodes share one machine's CPUs too, and the exchange's N(N - 1) messages land
# on them together, where on a cluster each lead's CPU carries its own. BUILD
# is a release build tree; OUTDIR gets places.txt, every osoe-bench line with
# its node count and each job's link and status, and summary.txt, the medians
# by node count, pattern and way, in microseconds.
#
# Every job runs under SHMEM_DEBUG, whose start-up line from PE 0 gives the link
# the job measured - from which its large reductions' shapes are chosen, as the
# default's way is - and places.txt keeps it; the summary names the way
# collective-shape.c's chooser takes on the default's job's link at the default
# settings - shape_timing.py's choose_places, which shmem.shape.places-grid
# holds to the C's - so that each node count's measured ways can be set against
# the choice.
#
# ROUNDS (1 by default) runs every node count's jobs that many times, the ways
# in turn each round, and the table gives the median of the rounds' medians:
# here, one job's median differed from the next's by as much as three times,
# as much as the ways differ, and H24 wants several.
#
# Each job is cut off at TIMEOUT seconds (600 by default) by the launcher's -t;
# a job cut off or failed leaves a line saying so, its stderr kept, and the
# table a dash, and the script says so and exits 1 at the end.
set -u
if [ $# -lt 2 ]; then
    echo "usage: collect-places.sh BUILD OUTDIR [NODES...]" >&2
    exit 2
fi
BUILD=$1
OUT=$2
shift 2
NODES=${*:-8 16 32 64 128 254}
LAUNCH=$BUILD/tools/launch/osoe-launch
BENCH=$BUILD/tools/bench/osoe-bench
HERE=$(cd "$(dirname "$0")" && pwd)
if [ ! -x "$LAUNCH" ] || [ ! -x "$BENCH" ]; then
    echo "collect-places.sh: no osoe-launch or osoe-bench under $BUILD" >&2
    exit 2
fi
mkdir -p "$OUT"
: > "$OUT/places.txt"
WAYS="members totals scan exchange link"
failed=0
for n in $NODES; do
    map=$(seq -s, 0 $((n - 1)))
    for round in $(seq 1 "${ROUNDS:-1}"); do
    for way in $WAYS; do
        job=$n-$way-$round
        env="SHMEM_SYMMETRIC_SIZE=8m OSOE_COLLECTIVE_PLACES=$way SHMEM_DEBUG=1"
        start=$(date +%s)
        if [ "${TIER3:-0}" = 1 ]; then
            # shellcheck disable=SC2086
            lines=$(sudo -n "$HERE/../launch/tier3.sh" pi "$n" $env -- "$LAUNCH" -n "$n" -m "$map" \
                        -t "${TIMEOUT:-600}" -- "$BENCH" places quick 2>"$OUT/stderr-$job.txt")
        else
            # shellcheck disable=SC2086
            lines=$(env $env "$LAUNCH" -n "$n" -m "$map" -t "${TIMEOUT:-600}" -- \
                        "$BENCH" places quick 2>"$OUT/stderr-$job.txt")
        fi
        status=$?
        seconds=$(( $(date +%s) - start ))
        if [ "$status" -ne 0 ]; then
            echo "collect-places.sh: the job of $n nodes by $way, round $round, ended with" \
                 "status $status ($OUT/stderr-$job.txt)" >&2
            failed=1
        fi
        echo "$lines" | grep '^places ' | sed "s/^/nodes=$n round=$round /" >> "$OUT/places.txt"
        echo "job nodes=$n way=$way round=$round status=$status seconds=$seconds" \
            >> "$OUT/places.txt"
        # PE 0's start-up line: "...; link measured: a hop of H ns, a message
        # cost of M ns, B bytes a second (...)". Then the debug lines go, every
        # PE's and every node's, and anything else stays.
        number='\([0-9]*\)'
        said="a hop of $number ns, a message cost of $number ns, $number bytes a second"
        as="link nodes=$n way=$way round=$round source=\\1 hop_ns=\\2 message_ns=\\3"
        as="$as bytes_per_second=\\4"
        grep '^osoe: PE 0 of ' "$OUT/stderr-$job.txt" |
            sed -n "s/.*; link \\([a-z]*\\): $said.*/$as/p" >> "$OUT/places.txt"
        grep -v -E -e '^osoe: PE [0-9]+ of [0-9]+ \(local ' \
            -e '^osoe: node [0-9]+(: .* resolves here | (limits|leaves) the unsent bytes )' \
            -e '^osoe: node [0-9]+ (probes its connections|sets no TCP keepalive) ' \
            "$OUT/stderr-$job.txt" > "$OUT/stderr-$job.kept"
        mv "$OUT/stderr-$job.kept" "$OUT/stderr-$job.txt"
        [ -s "$OUT/stderr-$job.txt" ] || rm -f "$OUT/stderr-$job.txt"
    done
    done
done
# The medians by node count and pattern, a column a way - of the rounds'
# medians, each job's - "-" where no job gave a line. Then the link each
# default's job measured, and the way the model chooses on it.
python3 - "$OUT/places.txt" "$HERE/../modelcheck" > "$OUT/summary.txt" <<'EOF'
import statistics
import sys
sys.path.insert(0, sys.argv[2])
from shape_timing import choose_places
rows, links, ways = {}, [], ['members', 'totals', 'scan', 'exchange', 'link']
for line in open(sys.argv[1]):
    f = dict(kv.split('=', 1) for kv in line.split() if '=' in kv)
    if line.startswith('nodes='):
        by_way = rows.setdefault((int(f['nodes']), f['lengths']), {})
        by_way.setdefault(f['way'], []).append(float(f['median_us']))
    elif line.startswith('link ') and f['way'] == 'link':
        links.append((int(f['nodes']), int(f['round']), int(f['hop_ns']), int(f['message_ns']),
                      int(f['bytes_per_second'])))
print('median microseconds a collect, one PE a node, the median of the rounds; the exchange '
      "past 64 nodes is each node's total, its room's fallback")
print(f"{'nodes':>5} {'lengths':<6} " + ' '.join(f'{w:>9}' for w in ways))
for (nodes, lengths), by_way in sorted(rows.items()):
    print(f'{nodes:>5} {lengths:<6} ' +
          ' '.join(f'{statistics.median(by_way[w]):>9.1f}' if w in by_way else f"{'-':>9}"
                   for w in ways))
if links:
    print()
    print("the link each default's job measured, and the way the default takes on it "
          '(the ceiling 8, the capacity 8, the small class 1 KiB; shape_timing.py)')
for nodes, round_, hop, message, rate in sorted(links):
    way = choose_places(nodes, nodes, True, True, min(nodes, 64), (hop, message, rate), 8, 8,
                        1024)
    print(f'{nodes:>5} round {round_}: a hop of {hop} ns, a message cost of {message} ns, '
          f'{rate} bytes a second: {way}')
EOF
cat "$OUT/summary.txt"
exit "$failed"
