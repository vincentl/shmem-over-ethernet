#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit set back for c4-standard-16, in the osoe VM: the kit's own dry run
# (docs/fast-link/dry-run.sh) - every section under osoe-launch's logical
# nodes at small sizes, then summarize.py over each section - from the tree
# vm-build.sh built, with the commit it was packed from as KIT_COMMIT. Under
# the VM's lock.
# usage: vm-dry-run.sh COMMIT OUTDIR
set -u
export KIT_COMMIT=$1
OUT=$2
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
cd /tmp/kit-c4/src || exit 1
echo "# lock taken $(date -u +%FT%TZ); $(cut -d' ' -f1-3 /proc/loadavg)"
rm -rf "${OUT:?}"
bash docs/fast-link/dry-run.sh "$OUT"
echo "dry-run.sh: $?"
