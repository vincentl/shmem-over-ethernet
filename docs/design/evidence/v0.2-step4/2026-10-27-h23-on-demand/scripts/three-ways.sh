#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The debug build's internode suite three ways, one after another, under the
# VM's lock: eager, on demand, and on demand at two progress helpers.
W=$(cd "$(dirname "$0")" && pwd)
bash "$W/suite.sh" eager debug OSOE_MESH_DATA_WIRES=eager -- -L internode -j4 --timeout 300
bash "$W/suite.sh" on-demand debug OSOE_MESH_DATA_WIRES=on-demand -- -L internode -j4 --timeout 300
bash "$W/suite.sh" on-demand-helpers debug OSOE_MESH_DATA_WIRES=on-demand OSOE_PROGRESS_HELPERS=2 -- -L internode -j4 --timeout 300
echo done > /tmp/h23-three-ways.status
