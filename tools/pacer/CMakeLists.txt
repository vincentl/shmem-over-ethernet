# SPDX-License-Identifier: LGPL-3.0-or-later
# osoe-pacer: puts the library through its paces - whole-job patterns from real
# OpenSHMEM codes, every result checked against an oracle
# (docs/design/osoe-pacer-proposal.md). Not installed. base/size.c is compiled
# in so that --memory and the heap's size parse exactly as the library parses
# SHMEM_SYMMETRIC_SIZE; libosoe hides its own copy.
add_executable(osoe-pacer osoe-pacer.c parallel-print.c sort.c alltoall.c mailbox.c teams.c
  threads.c print.c atomics.c
  ${PROJECT_SOURCE_DIR}/base/size.c)
target_include_directories(osoe-pacer PRIVATE ${PROJECT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(osoe-pacer PRIVATE osoe Threads::Threads m)
osoe_warnings(osoe-pacer)

# ---- The pacer's tests, in every preset: PENDING THE AUTHOR'S DECISION ------
#
# Whether these stay in every preset's suite, or move to a preset of their own
# run by hand, is the author's to decide once they have seen what they cost
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

  # The planted defects: each check must still fire - the brief's four, and
  # three more the author added on 2026-09-27 so that every check has one (the
  # capacity plant loses keys, so the oracle fires beside it). Every line a
  # plant's checks must print is required, each on its own, by
  # cmake/PacerPlant.cmake: several PEs' lines come in an order that depends
  # on who reserved first, and one pattern of "line, then count" would let
  # every check but the first go quiet unnoticed. The alltoall's totals say
  # that all eight variants caught it, every exchange. A `.` stands for each
  # `;` of a line, which would otherwise split the list of patterns.
  # Every planted run also says so, in a line after the header and on its
  # last line, so that it cannot be taken for a finding.
  function(pacer_plant defect exercise)
    set(patterns "osoe-pacer planted ${defect}: PE 1[ ']"
        "osoe-pacer failures=[1-9][0-9]* seconds=[0-9.]+ plant=${defect}" ${ARGN})
    add_test(NAME tools.pacer.plant.${defect}
      COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:osoe-launch>
              -DPACER=$<TARGET_FILE:osoe-pacer>
              "-DARGUMENTS=--quick;--plant;${defect};${exercise}" "-DPATTERNS=${patterns}"
              "-DFAIL=killed by signal|timeout after|left behind"
              -P ${PROJECT_SOURCE_DIR}/cmake/PacerPlant.cmake)
    set_tests_properties(tools.pacer.plant.${defect} PROPERTIES LABELS "tier2" TIMEOUT 300)
  endfunction()
  pacer_plant(sort-key sort
    "sort round=0 oracle: sum generated [0-9a-f]+, received [0-9a-f]+"
    "sort round=0 oracle: xor generated [0-9a-f]+, received [0-9a-f]+"
    "sort round=0 oracle: hash generated [0-9a-f]+, received [0-9a-f]+"
    "osoe-pacer failures=[1-9]")
  pacer_plant(sort-order sort
    "sort round=0 order: [0-9]+ keys follow a larger one. the first at [0-9]+ of [0-9]+"
    "osoe-pacer failures=[1-9]")
  pacer_plant(sort-range sort
    "sort round=0 range: 1 of [0-9]+ keys belong elsewhere. the first, [0-9a-f]+, is PE [0-9]+'s"
    "osoe-pacer failures=[1-9]")
  pacer_plant(sort-capacity sort
    "sort round=0 capacity: PE [0-9]+ holds [0-9]+ keys. a reservation of [0-9]+ reached [0-9]+"
    "sort round=0 capacity: reservations here reached [0-9]+ keys, past the [0-9]+ this PE holds. its batch goes unchecked"
    "sort round=0 oracle: count generated [0-9]+, received [0-9]+"
    "osoe-pacer failures=[1-9]")
  pacer_plant(alltoall-byte alltoall
    "alltoall round=0 team=world block=8 alltoall: 1 of [0-9]+ words wrong. the first, word 0 from PE 1, is [0-9a-f]+, not [0-9a-f]+: it matches no word sent"
    "summary alltoall rounds=12 exchanges=[0-9]+ seconds=[0-9.]+ exchanges_per_s=[0-9]+ failures=96"
    "osoe-pacer failures=96 ")
  pacer_plant(alltoall-gap alltoall
    "alltoall round=0 team=world block=8 alltoalls: 1 word outside the blocks written. the first at word 1 of dest, [0-9a-f]+"
    "summary alltoall rounds=12 exchanges=[0-9]+ seconds=[0-9.]+ exchanges_per_s=[0-9]+ failures=12"
    "osoe-pacer failures=12 ")
  # PE 0 records this failure after two others, PEs 2 and 3 after one: the
  # line under all three says the failure lines are grouped by content.
  # The mailbox's four: a payload word, a signal word's value, the count of
  # what arrived, which only the last checkpoint's pairwise check sees, and
  # both checks of a round trip across nodes - PE 1 serves PE 3 on 0,0,1,1.
  pacer_plant(mailbox-payload mailbox
    "mailbox round=0 wait_until_all from PE 1: 1 of [0-9]+ payload words wrong. the first, word 1, is [0-9a-f]+")
  pacer_plant(mailbox-signal mailbox
    "mailbox round=3 wait_until_all_vector from PE 1: signal 2, expected 1")
  pacer_plant(mailbox-count mailbox
    "mailbox round=19 count: PE [0-9]+ sent [0-9]+ here, [0-9]+ were read")
  pacer_plant(mailbox-round-trip mailbox
    "mailbox round=0 round_trip ping from PE 1: signal 2, expected 1"
    "mailbox round=0 round_trip ping from PE 1: 1 of 4 words wrong. the first, word 2, is [0-9a-f]+")
  # threads' three and teams' three; the exhaust plant needs its mode.
  pacer_plant(threads-row threads
    "threads round=0 private row: thread 0 read back word 3 as [0-9a-f]+, not [0-9a-f]+")
  pacer_plant(threads-table threads
    "threads round=0 [a-z]+ table: thread [0-9]+ got word 5 of PE 1's row [0-9]+ as [0-9a-f]+")
  pacer_plant(threads-count threads
    "threads round=0 counter [0-9]+: holds [0-9]+, and [0-9]+ were added")
  pacer_plant(teams-query teams
    "teams [a-z]+: team_n_pes [0-9]+ and team_my_pe [0-9]+, expected [0-9]+ and [0-9]+"
    "teams [a-z]+: rank 0 translates to world PE [0-9]+, expected [0-9]+"
    "teams [a-z]+: team_ptr to rank 0, world PE [0-9]+, is [A-Za-z ]+. it is "
    "teams [a-z]+: world PE 0 translates to rank -?[0-9]+, expected -?[0-9]+"
    "teams [a-z]+: team_get_config gave num_contexts 0, expected 1"
    "teams [a-z]+: a context of the team names another team"
    "teams row: this PE's row rank [0-9]+ translates to column rank [0-9]+, expected [0-9]+")
  pacer_plant(teams-collective teams
    "teams round=[0-9]+ row sum_reduce: element 0 is"
    "teams round=[0-9]+ row max_reduce: element 0 is 1000,"
    "teams round=[0-9]+ row min_reduce: element 0 is -1,"
    "teams round=[0-9]+ row prod_reduce: element 0 is"
    "teams round=[0-9]+ row and_reduce: element 0 is 0000000000000000,"
    "teams round=[0-9]+ row or_reduce: element 0 is ffffffffffffffff,"
    "teams round=[0-9]+ row xor_reduce: element 0 is"
    "teams round=[0-9]+ row double_sum_reduce: element 0 is"
    "teams round=[0-9]+ row broadcast: element 0 is"
    "teams round=[0-9]+ row collect: element 0 is"
    "teams round=[0-9]+ row fcollect: element [0-9]+ is"
    "teams round=[0-9]+ row alltoall: element [0-9]+ is"
    "teams round=[0-9]+ row sum_inscan: element 0 is"
    "teams round=[0-9]+ row sum_exscan: element 0 is")
  pacer_plant(teams-member teams
    "teams one: this PE is a member, and its handle is SHMEM_TEAM_INVALID")
  pacer_plant(teams-ring teams
    "teams round=0 strided context: read back [0-9]+ from rank [0-9]+, not [0-9]+")
  # The atomics' seven: each family's check, on the first round of its family
  # - int for the first five, unsigned int for the bitwise two.
  pacer_plant(atomics-add atomics
    "atomics round=0 add int default: slot [0-9]+ holds [0-9]+, expected [0-9]+")
  pacer_plant(atomics-fetch atomics
    "atomics round=12 fetch_add int default: slot [0-9]+ handed out values summing to [0-9]+, not the [0-9]+ of its [0-9]+ increments"
    "atomics round=12 fetch_add int default: slot [0-9]+ handed out values hashing to [0-9a-f]+, not [0-9a-f]+")
  pacer_plant(atomics-claim atomics
    "atomics round=24 claim int default: the table holds 63 claims, not the 64 made"
    "atomics round=24 claim int default: slot [0-9]+ of PE [0-9]+ holds [0-9]+, not this PE's claim [0-9]+")
  pacer_plant(atomics-swap atomics
    "atomics round=36 swap int default: the tokens sum to [0-9]+, not the [0-9]+ they started at"
    "atomics round=36 swap int default: the tokens hash to [0-9a-f]+, not the [0-9a-f]+ they started at")
  pacer_plant(atomics-set atomics
    "atomics round=50 set_fetch int serialized: fetched [0-9]+ from slot 0 of PE [0-9]+, which this PE set to [0-9]+"
    "atomics round=50 set_fetch int serialized: slot 0 holds [0-9]+, not the [0-9]+ PE 1 set")
  pacer_plant(atomics-bitwise atomics
    "atomics round=64 bitwise uint private: after the xor pass slot [0-9]+ holds [0-9a-f]+, expected [0-9a-f]+")
  pacer_plant(atomics-fetched-bit atomics
    "atomics round=64 bitwise uint private: fetch_or from slot [0-9]+ of PE [0-9]+ returned bit 1 clear, expected set")
  pacer_plant(teams-exhaust "teams;--teams-exhaust"
    "teams exhaust: this PE made [0-9]+ teams before a split failed, another [0-9]+")
  pacer_plant(print-hash print
    "print round=0 same: returned 2 lines, expected 1"
    ".0,2-3. print round=0 parity: returned 3 lines, expected 2"
    "print round=0 same: printed ..0,2-3. print round=0 same: every PE says this., expected ..0-3. print round=0 same: every PE says this."
    "osoe-pacer failures=[1-9]")

  # Splitting until the job's team numbers run out: the split must return
  # nonzero on every PE, and the job go on (design §5.7).
  add_test(NAME tools.pacer.teams-exhaust.0-0-1-1
    COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 240 -- $<TARGET_FILE:osoe-pacer> --teams-exhaust
            teams)
  set_tests_properties(tools.pacer.teams-exhaust.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "teams exhausted after [0-9]+ splits.*osoe-pacer failures=0 "
    FAIL_REGULAR_EXPRESSION "${pacer_failed}")

  # parallel_print checking its own lines, which pins the label format; on
  # 0,1,0,1 too, the one map whose shared teams' labels have gaps.
  foreach(map IN ITEMS 0,0,1,1 0,1,2,3 0,1,0,1)
    string(REPLACE "," "-" label "${map}")
    add_test(NAME tools.pacer.print.${label}
      COMMAND osoe-launch -n 4 -m ${map} -t 240 -- $<TARGET_FILE:osoe-pacer> --quick print)
    set_tests_properties(tools.pacer.print.${label} PROPERTIES LABELS "tier2" TIMEOUT 300
      PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "
      FAIL_REGULAR_EXPRESSION "${pacer_failed}")
  endforeach()
endif()
# threads, under SHMEM_THREAD_MULTIPLE, with both of the library's test hooks
# and a landing zone its gets overfill, as the library's own checked threaded
# test runs (shmem/tests/CMakeLists.txt): OSOE_CHECK_QUIET checks every shared
# wait's promise at its return, and OSOE_TEST_PERTURB widens the windows between
# a protocol's steps. On the four maps and on 0,1,2,0,1,0, where the hook's own
# race showed (docs/decisions/2026-09-27.md item 10); under tsan on 0,1, one PE a
# node, where every writer of a PE's memory is a thread of the process tsan sees.
set(pacer_threads_maps 0,0,0,0 0,1,2,3 0,0,0,1,1,2 0,1,0,1 0,1,2,0,1,0)
if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
  set(pacer_threads_maps 0,1)
endif()
foreach(map IN LISTS pacer_threads_maps)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME tools.pacer.threads.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1
            OSOE_GET_LANDING_BYTES=8192
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 240 --
            $<TARGET_FILE:osoe-pacer> --quick threads)
  set_tests_properties(tools.pacer.threads.${label} PROPERTIES LABELS "tier2" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "osoe-pacer failures=0 "
    FAIL_REGULAR_EXPRESSION "${pacer_failed}|OSOE_CHECK_QUIET:")
endforeach()
# ---- end of the pacer's tests ------------------------------------------------
