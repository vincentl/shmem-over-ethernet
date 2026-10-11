#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The integration branch's verification into its evidence directory: the
# scripts that ran it and its summary, the static checks', the models' and the
# pacer's whole outputs, and the ThreadSanitizer gate's verdicts, as the
# keepalive's verification was kept. Paths in what is copied are the VM's /tmp
# or relative; nothing names the host's.
# usage: copy-integration-evidence.sh REPOSITORY
set -eu
R=$1
M=$(cd "$(dirname "$0")" && pwd)
E=$R/docs/design/evidence/v0.2-step4/2026-10-10-integration
V=$M/verify-integration-4df0047
mkdir -p "$E/verify-4df0047"
cp "$M/verify-integration.sh" "$M/tsan-gate.py" "$M/copy-integration-evidence.sh" "$E/"
cp "$V/verify.txt" "$V/static.full" "$V/models.full" "$E/verify-4df0047/"
cp "$V"/pacer-*.txt "$E/verify-4df0047/"
for h in 2 3; do
    python3 -I "$M/tsan-gate.py" --timeout-explained shmem.stopped-bulk.0-0-1 \
        "$V/tsan-OSOE_PROGRESS_HELPERS_$h.full" > "$E/verify-4df0047/tsan-gate-$h.txt" 2>&1 || true
done
sed -i '' "s|$M/||g" "$E"/verify-4df0047/tsan-gate-*.txt
if grep -rn --exclude=copy-integration-evidence.sh '/Users/' "$E"; then echo "a host path is in the evidence" >&2; exit 1; fi
echo "copied $(find "$E" -type f | wc -l | tr -d ' ') files"
