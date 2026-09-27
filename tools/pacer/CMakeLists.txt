# SPDX-License-Identifier: LGPL-3.0-or-later
# osoe-pacer: puts the library through its paces - whole-job patterns from real
# OpenSHMEM codes, every result checked against an oracle
# (docs/design/osoe-pacer-proposal.md). Not installed. base/size.c is compiled
# in so that --memory and the heap's size parse exactly as the library parses
# SHMEM_SYMMETRIC_SIZE; libosoe hides its own copy.
add_executable(osoe-pacer osoe-pacer.c parallel-print.c sort.c alltoall.c print.c
  ${PROJECT_SOURCE_DIR}/base/size.c)
target_include_directories(osoe-pacer PRIVATE ${PROJECT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(osoe-pacer PRIVATE osoe Threads::Threads)
osoe_warnings(osoe-pacer)

# ---- The pacer's tests, in every preset: PENDING THE AUTHOR'S DECISION ------
#
# Whether these stay in every preset's suite, or move to a preset of their own
# run by hand, is the author's to decide once he has seen what they cost
# (proposal §3, "Tests"; docs/decisions/2026-09-26-night.md item 6). They are
# this one block: deleting it removes them all.
#
# The smoke tests: `--quick`, the sort and the alltoall at small sizes, over
# §8.1's four poles - one node, one PE a node, uneven, interleaved - so that
# every exercise crosses the intranode path, the internode path and their
# mixtures on every merge. Under ThreadSanitizer only the one-node map, as for
# every test over a node map: a write another process orders through a barrier
# looks like a race to it (tools/bench/CMakeLists.txt).
set(pacer_failed "exited with status [1-9]|killed by signal|timeout after|left behind")
set(pacer_maps 0,0,0,0 0,1,2,3 0,0,0,1,1,2 0,1,0,1)
if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
  set(pacer_maps 0,0,0,0)
endif()
foreach(map IN LISTS pacer_maps)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME tools.pacer.${label}
    COMMAND osoe-launch -n ${count} -m ${map} -t 240 -- $<TARGET_FILE:osoe-pacer> --quick)
  set_tests_properties(tools.pacer.${label} PROPERTIES LABELS "tier2" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "
    FAIL_REGULAR_EXPRESSION "${pacer_failed}")
endforeach()

if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  # The sort's other paths - the accumulating mode, the pull exchange, puts
  # where node-mates would take stores - so that every path of it runs too.
  add_test(NAME tools.pacer.sort-paths.0-0-1-1
    COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 240 -- $<TARGET_FILE:osoe-pacer> --quick
            --sort accumulate --exchange alltoall --no-ptr sort)
  set_tests_properties(tools.pacer.sort-paths.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "
    FAIL_REGULAR_EXPRESSION "${pacer_failed}")

  # The planted defects: each oracle must still fire. A run that found a
  # failure exits 1 by design, on every PE, so these run the launcher with -k -
  # a PE that exits first must not have the others killed in their finalize -
  # and fail only on a signal, the timeout or leftovers. The pattern is the
  # failure line, then the nonzero count: CMake's `.` crosses lines.
  foreach(plant IN ITEMS
      "sort-key|sort|sort round=0 oracle: sum generated [0-9a-f]+, received [0-9a-f]+"
      "sort-order|sort|sort round=0 order: [0-9]+ keys follow a larger one"
      "alltoall-byte|alltoall|alltoall round=0 team=world block=8 alltoall: 1 of [0-9]+ words wrong; the first, word 0 from PE 1, is [0-9a-f]+, not [0-9a-f]+: it matches no word sent"
      "print-hash|print|print round=0 same: returned 2 lines, expected 1")
    string(REPLACE "|" ";" plant "${plant}")
    list(GET plant 0 defect)
    list(GET plant 1 exercise)
    list(GET plant 2 line)
    add_test(NAME tools.pacer.plant.${defect}
      COMMAND osoe-launch -k -n 4 -m 0,0,1,1 -t 240 -- $<TARGET_FILE:osoe-pacer> --quick
              --plant ${defect} ${exercise})
    set_tests_properties(tools.pacer.plant.${defect} PROPERTIES LABELS "tier2" TIMEOUT 300
      PASS_REGULAR_EXPRESSION "${line}.*osoe-pacer failures=[1-9]"
      FAIL_REGULAR_EXPRESSION "killed by signal|timeout after|left behind")
  endforeach()

  # parallel_print checking its own lines, which pins the label format.
  foreach(map IN ITEMS 0,0,1,1 0,1,2,3)
    string(REPLACE "," "-" label "${map}")
    add_test(NAME tools.pacer.print.${label}
      COMMAND osoe-launch -n 4 -m ${map} -t 240 -- $<TARGET_FILE:osoe-pacer> --quick print)
    set_tests_properties(tools.pacer.print.${label} PROPERTIES LABELS "tier2" TIMEOUT 300
      PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "
      FAIL_REGULAR_EXPRESSION "${pacer_failed}")
  endforeach()
endif()
# ---- end of the pacer's tests ------------------------------------------------
