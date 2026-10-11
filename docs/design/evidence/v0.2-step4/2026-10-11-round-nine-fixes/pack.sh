#!/bin/zsh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Pack the fixes worktree as it stands, uncommitted changes and new files
# included, ignored files not: no AppleDouble files (COPYFILE_DISABLE).
set -e
W=<repo>/build/scratch/wt/review-nine-fixes
OUT=${1:-<repo>/build/scratch/review-nine-fixes/tree.tar}
cd $W
git ls-files -co --exclude-standard -z | COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata --null -cf $OUT -T -
echo "packed $(tar -tf $OUT | wc -l | tr -d ' ') files into $OUT"
