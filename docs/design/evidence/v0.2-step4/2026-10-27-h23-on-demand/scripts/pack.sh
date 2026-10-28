#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Pack the worktree for the VM, without AppleDouble files or macOS metadata.
cd "$(dirname "$0")/../../.." && COPYFILE_DISABLE=1 tar --no-xattrs --no-mac-metadata -cf build/scratch/h23/tree.tar --exclude=./build --exclude=./.git .
