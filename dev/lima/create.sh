#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Create and start an osoe development VM.
#
# The repository is mounted writable at the same absolute path it has on the
# host. Commands run from the repository root with `limactl shell <name> ...`
# therefore start in the same directory inside the VM, and paths in build trees
# and compile_commands.json are valid on both sides.
#
# The definition is <instance-name>.yaml beside this script where there is one -
# osoe-rocky.yaml, the Rocky Linux 9 VM the single-node build is tested in - and
# osoe.yaml, the Debian 13 VM, otherwise.
#
# Usage: dev/lima/create.sh [instance-name]    (default: osoe)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
name=${1:-osoe}
definition="$here/$name.yaml"
[ -f "$definition" ] || definition="$here/osoe.yaml"

limactl create --name="$name" --mount-only "$repo:w" --tty=false "$definition"
limactl start "$name"
