#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copy the merge point's scripts and outputs into the evidence tree of the
# branch that merges (the lead's, on the host), every script given the
# licence line second under its shebang, and the paths written as <repo>.
# usage: copy-evidence.sh WORKTREE
set -e
W=$1; M=$(cd "$(dirname "$0")" && pwd)
E=$W/docs/design/evidence/v0.2-step4/2026-10-10-merge-point
[ -d "$W/docs/design/evidence/v0.2-step4" ] || { echo "no evidence tree in $W"; exit 1; }
mkdir -p "$E"
for f in presets-helpers.sh sos.sh acceptance.sh spam-repro.sh pi-compare.sh pi-compare.py \
         building.sh container-spot.sh verify.sh helper-wake-check.sh verify-final.sh copy-evidence.sh \
         pi-vs-step3-comparison.txt; do
  cp "$M/$f" "$E/"
done
cp -R "$M/presets" "$M/sos" "$M/acceptance" "$M/spam" "$M/spam-quiet" "$M/pi-vs-step3" "$E/"
mkdir -p "$E/building" "$E/verify-4f1014d"
cp "$M/building/building.txt" "$M/building/models.txt" "$M/building/container-failed.txt" \
   "$M/building/container-spot.txt" "$E/building/"
V=$M/verify-4f1014d
cp "$V/verify.txt" "$V/static.full" "$V/models.full" "$V"/pacer-*.txt "$V"/tsan-*-classified.txt \
   "$E/verify-4f1014d/"
grep -n -A12 'helper-wake.0-1-2-3 \.\.\..*Failed' "$V/asan-OSOE_PROGRESS_HELPERS_0.full" \
   > "$E/verify-4f1014d/asan-helper-wake-failure.txt" || true
if [ -d "$M/verify-final" ]; then
  mkdir -p "$E/verify-final"
  cp "$M/verify-final/verify.txt" "$M/verify-final/static.full" "$E/verify-final/"
  cp "$M/verify-final"/helper-wake-*.full "$M/verify-final/single-node-info.txt" "$E/verify-final/" 2>/dev/null || true
fi
if [ -d "$M/helper-wake-check" ]; then
  mkdir -p "$E/helper-wake-check"
  cp "$M/helper-wake-check/helper-wake-check.txt" "$E/helper-wake-check/"
  for f in asan-loaded-1.txt mutant-spin-200000-ns-1.txt mutant-spin-30000-ns-1.txt; do
    [ -f "$M/helper-wake-check/$f" ] && cp "$M/helper-wake-check/$f" "$E/helper-wake-check/"
  done
fi
# The licence line where a script lacks it: second under a shebang, else first.
for f in "$E"/*.sh "$E"/*.py; do
  grep -q 'SPDX-License-Identifier: LGPL-3.0-or-later' "$f" || echo "$f"
done > "$M/license-missing.txt"
(cd "$W" && python3 "$M/add-license-line.py" "$M/license-missing.txt" > /dev/null)
# The repository's directory as <repo>, the home directory as ~.
grep -rl "$HOME" "$E" | while read -r f; do
  sed -i '' "s#$HOME/shmem-over-ethernet#<repo>#g; s#$HOME#~#g" "$f"
done
echo "copied: $(find "$E" -type f | wc -l) files, $(du -sh "$E" | cut -f1)"
grep -rl "$HOME" "$E" || echo "no paths under the home directory"
