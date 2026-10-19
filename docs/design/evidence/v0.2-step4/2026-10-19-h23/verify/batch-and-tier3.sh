#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H23 verified whole: the batch, then tier 3s suite and the mesh models.
/tmp/batch-verify.sh /tmp/h23-verify
cd /tmp/h23-verify || exit 1
ctest --test-dir build/tier3 -L tier3 -j2 --timeout 600 > /tmp/h23-verify-tier3.txt 2>&1
echo "tier3: $(grep -E "tests passed" /tmp/h23-verify-tier3.txt)"; grep -E "\(Failed\)|\(Timeout\)" /tmp/h23-verify-tier3.txt | head -8
ctest --test-dir build/release -R "^model[.]mesh" --timeout 600 > /tmp/h23-verify-models.txt 2>&1
echo "mesh models: $(grep -E "tests passed" /tmp/h23-verify-models.txt)"
echo "H23 VERIFY DONE $(date +%H:%M:%S)"
