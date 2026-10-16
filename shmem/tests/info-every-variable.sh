#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# SHMEM_INFO's text names every variable of the README's tables of settings, the
# transport's buffers and the testing hooks among them (D29: it named 21 of the
# 34 the tables had in round four, none of the transport's buffers):
#
#   info-every-variable.sh README LAUNCHER PROGRAM
#
# reads the names from the first column of every table of README's
# "Environment variables" section and requires each, as a whole word, in what a
# one-PE job of PROGRAM writes under SHMEM_INFO=1.
set -u
README=$1 LAUNCHER=$2 PROGRAM=$3
names=$(sed -n '/^### Environment variables$/,/^### /p' "$README" | grep '^| `' |
    sed 's/^| \([^|]*\)|.*/\1/' | grep -o '`[A-Z][A-Z0-9_]*`' | tr -d '`' | sort -u)
count=$(printf '%s\n' "$names" | grep -c .)
if [ "$count" -lt 30 ]; then
    echo "info-every-variable.sh: $count names in $README's tables: the scan is broken"
    exit 1
fi
out=$(SHMEM_INFO=1 "$LAUNCHER" -n 1 -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
missing=0
for name in $names; do
    if ! printf '%s\n' "$out" | grep -qw -- "$name"; then
        echo "info-every-variable.sh: SHMEM_INFO does not name $name"
        missing=$((missing + 1))
    fi
done
echo "info-every-variable.sh: the job exited $status; $count variables in the README's tables, $missing not named"
[ "$status" -eq 0 ] && [ "$missing" -eq 0 ] && echo "INFO NAMES EVERY VARIABLE"
