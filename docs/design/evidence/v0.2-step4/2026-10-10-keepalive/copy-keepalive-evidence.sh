#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copy keepalive's checks into the evidence tree of the branch that merges -
# the mutant run at e2bb1e9, the verification at e2bb1e9 and the final check
# at the tip - every script given the licence line second under its shebang,
# and the paths written as <repo>.
# usage: copy-keepalive-evidence.sh WORKTREE
set -e
W=$1; M=$(cd "$(dirname "$0")" && pwd)
E=$W/docs/design/evidence/v0.2-step4/2026-10-10-keepalive
[ -d "$W/docs/design/evidence/v0.2-step4" ] || { echo "no evidence tree in $W"; exit 1; }
mkdir -p "$E/keepalive-check" "$E/verify-e2bb1e9" "$E/verify-final-9b6ba72" "$E/verify-final"
cp "$M/keepalive-check.sh" "$M/verify.sh" "$M/verify-keepalive-final.sh" "$M/copy-keepalive-evidence.sh" \
   "$M/tsan-gate.py" "$E/"
cp "$M/keepalive-check/keepalive-check.txt" "$M/keepalive-check/mutant.full" "$E/keepalive-check/"
V=$M/verify-keepalive
cp "$V/verify.txt" "$V/static.full" "$V/models.full" "$V"/pacer-*.txt "$E/verify-e2bb1e9/"
cp "$V"/tsan-*-classified.txt "$E/verify-e2bb1e9/" 2>/dev/null || true
# The final check twice: at 9b6ba72, where the test's own defect failed both
# keepalive tests, and at the rebased tip with it fixed.
F=$M/verify-keepalive-final
cp "$F/verify.txt" "$F/release-keepalive.full" "$E/verify-final-9b6ba72/"
F=$M/verify-keepalive-final-2
cp "$F/verify.txt" "$F/static.full" "$F"/*-keepalive.full "$F/tier3.full" "$F"/q2-1-*.full \
   "$F"/mutant-*.full "$E/verify-final/"
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
