#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# compare.sh: T4's SOS runs against round ten's of the morning (build/scratch/review-ten/conformance/sos.txt,
# 5cefc7fb, the defaults, the same map 0,0,1,1,2,2,3,3): the programs whose exit status differs, and those
# in one run and not the other.
HERE=$(cd "$(dirname "$0")" && pwd)
R10=$HERE/../../review-ten/conformance/sos.txt
cd "$HERE" || exit 1
awk '$2 == "0-0-1-1-2-2-3-3" {print $1, $4}' "$R10" | sort > .r10.pairs
grep 'BUILD FAILED' "$R10" | awk '{print $1, "build-failed"}' | sort >> .r10.pairs
sort -o .r10.pairs .r10.pairs
for H in 0 2; do
  f=sos-h$H.txt
  [ -f "$f" ] || continue
  awk '$2 == "0-0-1-1-2-2-3-3" {print $1, $4}' "$f" | sort > .h$H.pairs
  grep 'BUILD FAILED' "$f" | awk '{print $1, "build-failed"}' | sort >> .h$H.pairs
  sort -o .h$H.pairs .h$H.pairs
  echo "== helpers $H against round ten (default helpers): programs $(wc -l < .h$H.pairs) against $(wc -l < .r10.pairs)"
  diff .r10.pairs .h$H.pairs && echo "   identical: the same programs, the same exit statuses (round ten's on the left, T4's on the right above if any differ)"
done
if [ -f .h0.pairs ] && [ -f .h2.pairs ]; then
  echo "== helpers 0 against helpers 2"
  diff .h0.pairs .h2.pairs && echo "   identical"
fi
rm -f .r10.pairs .h0.pairs .h2.pairs
