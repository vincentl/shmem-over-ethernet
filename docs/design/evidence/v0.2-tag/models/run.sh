#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T1's models on the host, the tree to be tagged's tools/modelcheck: each as the suite runs it,
# three at a time; each one's exit and minutes. A model's own report says what it checked.
cd "$(dirname "$0")/tree/tools/modelcheck" || exit 2
one() { local name=$1; shift; local s=$(date +%s); python3 "$@" > "../../../out-$name.txt" 2>&1; local st=$?
        echo "$name exit $st $(( ($(date +%s) - s) / 60 )) min"; }
export -f one
while read -r name rest; do echo "$name $rest"; done < ../../../jobs.txt | xargs -P 3 -L 1 bash -c 'one "$@"' _ > ../../../results.txt
echo "DONE $(grep -c ' exit 0 ' ../../../results.txt) of $(wc -l < ../../../results.txt) exit 0" >> ../../../results.txt
