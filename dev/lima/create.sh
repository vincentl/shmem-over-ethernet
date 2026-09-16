#!/bin/sh
# Create and start the osoe development VM.
#
# The repository is mounted writable at the same absolute path it has on the
# host. Commands run from the repository root with `limactl shell <name> ...`
# therefore start in the same directory inside the VM, and paths in build trees
# and compile_commands.json are valid on both sides.
#
# Usage: dev/lima/create.sh [instance-name]    (default: osoe)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
name=${1:-osoe}

limactl create --name="$name" --mount-only "$repo:w" --tty=false "$here/osoe.yaml"
limactl start "$name"
