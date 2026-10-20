#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T46: how this directory was made from the plan's output (t46-plan.sh).
#
#   t46-collect.sh SOURCE DEST
#
# SOURCE is a copy of the plan's OUTPUT directory on a host with python3 - the plan
# ran in the osoe VM with OUTPUT ~/t46-out, and the copy was made with
#   limactl shell osoe bash -c 'cd ~/t46-out && tar -c .' | tar -x -C SOURCE
# - and DEST is this directory. It
#   - runs the reports over the whole of SOURCE (the captures are 1.2 MB each, so
#     only some are kept below, and the reports are the record of the rest),
#   - copies each run's counters, its standard error (gzipped) and its job record
#     into DEST/runs/SERIES/run-NN, and the captures of capture/run-01, capture/run-03
#     and portloss/run-01 (xz) - the two captured runs where a SYN and a SYN-ACK were
#     lost, and the control that loses frames on purpose,
#   - leaves out what was empty in every run: dmesg.txt (the kernel wrote nothing
#     during any job) and wrapper.log (the helpers said nothing), the plan's
#     configure and build output (0 warnings, in progress.txt), and the
#     link0-addresses.txt of the static series.
set -eu
SOURCE=$1
DEST=$2
HERE=$(cd "$(dirname "$0")" && pwd)
REPORT="python3 -I $HERE/t46-report.py"
CAPTURE="python3 -I $HERE/t46-capture-report.py"
mkdir -p "$DEST/runs" "$DEST/reports"

cd "$SOURCE"
series=$(ls -d */ | tr -d / | sort)

cp environment.txt progress.txt "$DEST/"
mkdir -p "$DEST/runs/logs"
cp series-*.log "$DEST/runs/logs/"
for name in $series; do
    mkdir -p "$DEST/runs/$name"
    cp "$name/series.txt" "$DEST/runs/$name/"
    for run in "$name"/run-*; do
        target=$DEST/runs/$run
        mkdir -p "$target"
        cp "$run/before.json.gz" "$run/after.json.gz" "$run/job.txt" "$run/tier3-status.txt" "$target/"
        gzip -9nc "$run/err.txt" > "$target/err.txt.gz"
        [ ! -s "$run/out.txt" ] || cp "$run/out.txt" "$target/"
        [ ! -e "$run/port-netem.txt" ] || cp "$run/port-netem.txt" "$target/"
    done
done
for run in capture/run-01 capture/run-03 portloss/run-01; do
    python3 -I -c 'import gzip, lzma, sys
sys.stdout.buffer.write(lzma.compress(gzip.open(sys.argv[1], "rb").read(), preset=9 | lzma.PRESET_EXTREME))' \
        "$run/capture.txt.gz" > "$DEST/runs/$run/capture.txt.xz"
done

# The reports, run from SOURCE with relative names so that no path of a machine is in them.
$REPORT --series . > "$DEST/series-summary.txt"
for name in $series; do
    $REPORT "$name"/run-* > "$DEST/reports/counters-$name.txt"
done
# The nodes against what moved in them, over every run at the default backlog that lost no frame
# on purpose: the baseline, the captured runs, the default half of the A/B, and the static runs.
$REPORT base/run-* capture/run-* ab-default/run-* static/run-* | sed -n '/^The nodes of/,$p' \
    > "$DEST/reports/counters-nodes-at-default-backlog.txt"
$REPORT lossy/run-* portloss/run-* | sed -n '/^The nodes of/,$p' > "$DEST/reports/counters-nodes-with-loss-injected.txt"
$CAPTURE smoke/run-01 capture/run-0[1-8] portloss/run-0[12] > "$DEST/reports/packets-stories.txt"
$CAPTURE --totals capture/run-0[1-8] > "$DEST/reports/packets-totals.txt"
$CAPTURE --totals portloss/run-0[12] > "$DEST/reports/packets-totals-portloss.txt"
