#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# summarize.sh: a digest of T4's outputs in this directory, for the report - on the Mac or in the VM.
#   summarize.sh > summary.txt
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE" || exit 1
echo "== progress"; grep -E "lock held|phase|all phases|DONE|NO LOCK|stray" progress.log
for H in 0 2; do
  f=sos-h$H.txt
  [ -f "$f" ] || continue
  echo; echo "== SOS at 4 x 2 (0,0,1,1,2,2,3,3), OSOE_PROGRESS_HELPERS=$H"
  head -1 "$f"; sed -n '/^== summary/,/^DONE/p' "$f"
  echo "-- runs not exiting 0:"; awk '$2 ~ /^0-/ && $4 != 0' "$f" | cut -c1-260
  echo "-- builds failing:"; grep 'BUILD FAILED' "$f"
done
for H in 0 2; do
  f=teams70-h$H.txt
  [ -f "$f" ] || continue
  echo; echo "== team programs at 70 PEs, OSOE_PROGRESS_HELPERS=$H"
  head -1 "$f"; cat "$f" | sed -n '2,$p' | cut -c1-230
done
for H in 0 2; do
  f=sos-repeat/sos-h$H.txt
  [ -f "$f" ] || continue
  echo; echo "== SOS at 4 x 2, a second pass (sos-repeat/), OSOE_PROGRESS_HELPERS=$H"
  head -1 "$f"; sed -n '/^== summary/,/^DONE/p' "$f"
  echo "-- runs not exiting 0:"; awk '$2 ~ /^0-/ && $4 != 0 {print $1, "exit", $4}' "$f"
  echo "-- builds failing:"; grep 'BUILD FAILED' "$f" | awk '{print $1}'
done
if [ -f teams70-repeats.txt ]; then echo; echo "== 70-PE team programs, three more passes at each helper count (teams70-repeats/)"; cat teams70-repeats.txt; fi
for f in helpers-threads.txt helpers-check.txt; do
  [ -f "$f" ] && { echo; echo "== $f"; cat "$f" | cut -c1-200; }
done
echo; echo "== what else used the VM (load-sampler.py; samples 10 s apart, load-teams-repeats.log 1 s apart)"
echo "   machine = cores busy in all; T4 = those of T4's own processes alive across a sample (short-lived programs are mostly missed, so low);"
echo "   others = cores used by live processes outside T4's own"
for f in load-*.log; do
  [ -f "$f" ] || continue
  awk -v name="$f" '
    /load1=/ {
      split($2,a,"="); split($3,b,"="); split($4,c,"="); split($5,d,"=");
      m=b[2]+0; j=c[2]+0; o=d[2]+0;
      n++; sm+=m; sj+=j; so+=o; if (o>mo) mo=o; if (m>mm) mm=m; if (o>=0.25) hi++;
      for (i = 6; i <= NF; i++) if ($i ~ /^[0-9]+:[0-9.]+:/) { x = $i; sub(/^[0-9]+:[0-9.]+:/, "", x); who[x] = 1 }
    }
    END { if (n) { names = ""; for (x in who) names = names " " x; printf "%s: %d samples; machine mean %.2f max %.2f cores; T4 mean %.2f; others mean %.2f max %.2f; samples with others >= 0.25 core: %d; outside processes seen at 0.10 core or more:%s\n", name, n, sm/n, mm, sj/n, so/n, mo, hi, (names == "" ? " none" : names) } }' "$f"
done
