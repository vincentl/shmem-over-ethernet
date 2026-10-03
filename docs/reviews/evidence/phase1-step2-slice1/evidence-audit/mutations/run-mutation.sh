#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# usage: run-mutation.sh <name> <ctest -R regex> <file relative to tree> <perl -0pi expression>
# Applies one mutation to the PRIVATE copy, records the diff, rebuilds debug, runs the
# selected tests, then restores the file from the real tree. Never touches the real tree.
set -u
REAL=<repo>
TREE=$REAL/build/scratch/review-evidence-audit/tree
OUT=$REAL/docs/reviews/evidence/phase1-step2-slice1/evidence-audit/mutations
name=$1; regex=$2; file=$3; expr=$4
cp "$REAL/$file" "$TREE/$file"
perl -0pi -e "$expr" "$TREE/$file"
diff -u "$REAL/$file" "$TREE/$file" > "$OUT/$name.diff"
if [ ! -s "$OUT/$name.diff" ]; then echo "$name: MUTATION DID NOT APPLY"; exit 2; fi
limactl shell osoe sh -c "cd $TREE && cmake --build build/debug 2>&1 | grep -E 'Building|error|warning'  && cd build/debug && ctest -R '$regex' --timeout 120 --output-on-failure 2>&1 | grep -v 'left behind\|wire split' | tail -150" > "$OUT/$name.log" 2>&1
cp "$REAL/$file" "$TREE/$file"; touch "$TREE/$file"
grep -E "tests passed|\*\*\*|Failed|Timeout|error:" "$OUT/$name.log" | head -30
