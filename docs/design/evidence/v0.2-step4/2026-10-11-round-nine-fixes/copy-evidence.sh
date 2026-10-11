#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's changes' evidence into the tree: the full verification's summary
# and its static checks', models', pacer's and gate's outputs; the scripts
# behind the day's checks - the gate's plants and synthetic reports, the
# ThreadSanitizer suite's runs, a node-mate started late, the Pi's tier-2 runs -
# with what they printed. Paths become <repo>; a script without a licence line
# is given one. Usage: copy-evidence.sh (from anywhere).
set -eu
S=<repo>/build/scratch/review-nine-fixes
E=<repo>/build/scratch/wt/review-nine-fixes/docs/design/evidence/v0.2-step4/2026-10-11-round-nine-fixes
rm -rf "$E"; mkdir -p "$E"/verify "$E"/tsan "$E"/gate-check "$E"/late-rank "$E"/pi
V=$S/verify-234b622
cp $V/verify.txt $V/static.full $V/models.full $V/pacer-*.txt $V/tsan-gate-*.txt "$E"/verify/
cp $S/verify-fixes.sh $S/pack.sh $S/quick-tsan.sh $S/quick-debug.sh $S/full-tsan.sh $S/head-check.sh $S/copy-evidence.sh "$E"/
cp $S/logs/full-tsan/gate-h*.txt $S/logs/full-tsan/gate3-h*.txt "$E"/tsan/
cp $S/gate-check/plant.py $S/gate-check/synthetic.py $S/gate-check/run.sh $S/gate-check/r9f.sh "$E"/gate-check/
cp $S/gate-check/logs/*.txt "$E"/gate-check/
cp $S/late-rank/*.sh $S/late-rank/*.txt "$E"/late-rank/
cp $S/pi/*.sh $S/pi/run.txt $S/pi/flaky.txt $S/pi/ab.txt $S/pi/held.txt $S/pi/diag.txt $S/pi/serial.txt \
   $S/pi/lead-ends-diag.txt $S/pi/lead-ends-ps.txt "$E"/pi/
find "$E" -type f | while read f; do
  sed -i '' -e 's#<repo>#<repo>#g' -e 's#~#~#g' "$f"
  case "$f" in *.sh|*.py)
    grep -q SPDX-License-Identifier "$f" || sed -i '' '1a\
# SPDX-License-Identifier: LGPL-3.0-or-later
' "$f";;
  esac
done
echo "$(find "$E" -type f | wc -l | tr -d ' ') files; private paths left: $(grep -rl '/Users/' "$E" | wc -l | tr -d ' ')"
