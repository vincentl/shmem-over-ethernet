#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The whole verification, then tier 3 at 64 logical nodes on its tree.
W=$(cd "$(dirname "$0")" && pwd)
bash "$W/verify.sh" "$W/tree.tar" h23-verify '^model\.(data_wire|mesh|mesh\.digest|mesh_timing|lead_wake|visit|selftest)$'
bash "$W/tier3-64.sh" /var/tmp/h23-verify 2
