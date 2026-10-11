#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, the check of S1's fix: the kit's multistream.c with
# one block added after the window - when MULTISTREAM_LOG names a directory,
# each PE writes its records there as the S1 refuter's multistream-log did
# (pe-ME.txt: a header, then a line a block: call start, call return, the end
# of the quiet after it or the return again, peer node), so that analyze.py's
# exact counts, computed offline from the same records, can be set beside the
# figures the probe swept on each node's first PE. Nothing else changes.
# usage: make-check-probe.py MULTISTREAM_C OUT_C
import sys

source = open(sys.argv[1]).read()
anchor = "        shmem_quiet();\n    }\n    double elapsed = now() - start;\n"
assert source.count(anchor) == 1, "the anchor after the window moved"
block = r'''        shmem_quiet();
    }
    {   /* The check's log, in analyze.py's form (make-check-probe.py). */
        const char *log_directory = getenv("MULTISTREAM_LOG");
        if (log_directory && records) {
            char path[4096];
            snprintf(path, sizeof path, "%s/pe-%d.txt", log_directory, me);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "pe %d node %d rank %d nodes %d start %.9f stop %.9f slice %.9f "
                           "blocks %llu dropped %d\n",
                        me, mine, my_rank, nodes_count, start, stop, 1e-3,
                        (unsigned long long)recorded, recorded == room);
                for (uint64_t i = 0; i < recorded; i++)
                    fprintf(f, "%.9f %.9f %.9f %d\n", records[i].begin, records[i].end,
                            records[i].end + records[i].quiet, records[i].peer);
                fclose(f);
            }
        }
    }
    double elapsed = now() - start;
'''
open(sys.argv[2], "w").write(source.replace(anchor, block))
