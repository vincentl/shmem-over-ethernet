add_executable(shmem_test_info test-info.c)
target_link_libraries(shmem_test_info PRIVATE osoe)
osoe_warnings(shmem_test_info)
add_test(NAME shmem.info COMMAND shmem_test_info)
set_tests_properties(shmem.info PROPERTIES LABELS "tier1" TIMEOUT 30)

if(BUILD_SHARED_LIBS)
  add_test(NAME shmem.exports
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:osoe>
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckExports.cmake)
  set_tests_properties(shmem.exports PROPERTIES LABELS "tier1" TIMEOUT 30)

  # The profiling interface must be strong pshmem_ + weak shmem_ (spec §10).
  # A behavioural test cannot see this: in a shared-library build the
  # executable's definition preempts the library's whatever its binding.
  add_test(NAME shmem.profiling_aliases
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:osoe>
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckProfiling.cmake)
  set_tests_properties(shmem.profiling_aliases PROPERTIES LABELS "tier1" TIMEOUT 30)
endif()

# The allocator on its own (tier 1): compiled in, since libosoe hides it.
add_executable(shmem_test_allocator test-allocator.c ../src/allocator.c)
target_include_directories(shmem_test_allocator PRIVATE ../src)
osoe_warnings(shmem_test_allocator)
add_test(NAME shmem.allocator COMMAND shmem_test_allocator)
set_tests_properties(shmem.allocator PROPERTIES LABELS "tier1" TIMEOUT 60)

# The §4.9 watched-range encoding on its own (tier 1): pure logic, and the
# property it must have is one a brute-force check can state directly.
add_executable(shmem_test_watch_range test-watch-range.c)
target_include_directories(shmem_test_watch_range PRIVATE ../src)
osoe_warnings(shmem_test_watch_range)
add_test(NAME shmem.watch-range COMMAND shmem_test_watch_range)
set_tests_properties(shmem.watch-range PROPERTIES LABELS "tier1" TIMEOUT 60)

# AM framing and the stream parser (tier 1): the same bytes in every split.
add_executable(shmem_test_am test-am.c ../src/am.c)
target_include_directories(shmem_test_am PRIVATE ../src)
target_link_libraries(shmem_test_am PRIVATE osoe_base)
osoe_warnings(shmem_test_am)
add_test(NAME shmem.am COMMAND shmem_test_am)
set_tests_properties(shmem.am PROPERTIES LABELS "tier1" TIMEOUT 120)

# A framing error must be a loud, immediate abort naming the field that
# disagreed (§3.4, §3.15) - not a garbage dispatch later. These assert the
# message, so a check that stopped firing fails here rather than going quiet.
foreach(case_expect
    "count|carried 4 messages, its header says 5"
    "length|was [0-9]+ bytes, its header says"
    "short-segment|less than its own header"
    "kind|this build knows"
    "short-message|less than its fixed part")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 case)
  list(GET parts 1 expect)
  add_test(NAME shmem.am.malformed.${case} COMMAND shmem_test_am ${case})
  set_tests_properties(shmem.am.malformed.${case} PROPERTIES
    LABELS "tier1" TIMEOUT 60 PASS_REGULAR_EXPRESSION "${expect}")
endforeach()

# Deferred reply queues and the credit (tier 1): pure logic and one atomic.
add_executable(shmem_test_reply test-reply.c ../src/reply.c)
target_include_directories(shmem_test_reply PRIVATE ../src)
target_link_libraries(shmem_test_reply PRIVATE osoe_base Threads::Threads)
osoe_warnings(shmem_test_reply)
add_test(NAME shmem.reply COMMAND shmem_test_reply)
set_tests_properties(shmem.reply PROPERTIES LABELS "tier1" TIMEOUT 60)

# The AM sizing policy (tier 1): pure arithmetic, compiled in.
add_executable(shmem_test_am_size test-am-size.c ../src/connection-area.c ../src/connection.c)
target_include_directories(shmem_test_am_size PRIVATE ../src)
target_link_libraries(shmem_test_am_size PRIVATE osoe_base rt)
osoe_warnings(shmem_test_am_size)
add_test(NAME shmem.am-size COMMAND shmem_test_am_size)
set_tests_properties(shmem.am-size PROPERTIES LABELS "tier1" TIMEOUT 60)

# The reservation protocol (tier 1): the real code under real threads, checking
# the properties the models check. Writers are threads because that is what the
# tsan preset can instrument; §8.2 says what that does and does not buy.
add_executable(shmem_test_connection test-connection.c ../src/connection.c)
target_include_directories(shmem_test_connection PRIVATE ../src)
target_link_libraries(shmem_test_connection PRIVATE osoe_base Threads::Threads)
osoe_warnings(shmem_test_connection)
add_test(NAME shmem.connection COMMAND shmem_test_connection)
set_tests_properties(shmem.connection PROPERTIES LABELS "tier1" TIMEOUT 120)

# The node-pair wire on its own (tier 1): both ends in one process, so no
# launcher. wire.c is compiled in, since libosoe hides it, and the test supplies
# its own osoe_fatal so a failure inside it fails the test.
add_executable(shmem_test_wire test-wire.c ../src/wire.c)
target_include_directories(shmem_test_wire PRIVATE ../src)
target_link_libraries(shmem_test_wire PRIVATE osoe_base rt)
osoe_warnings(shmem_test_wire)
add_test(NAME shmem.wire COMMAND shmem_test_wire)
set_tests_properties(shmem.wire PROPERTIES LABELS "tier1" TIMEOUT 60)

# The wire between two processes (tier 2). Two logical nodes, one PE each, so
# each process is a node lead - the only role that touches a wire (§3.12). It
# does not call shmem_init: it tests the wire on its own, below the library,
# and takes its identity straight from the launcher environment.
# The seed is fixed: §3.14 wants an injected fault reproducible, and without it
# the pattern would follow the launcher's pid and differ every run.
add_executable(shmem_test_wire_pair test-wire-pair.c ../src/wire.c)
target_include_directories(shmem_test_wire_pair PRIVATE ../src)
target_link_libraries(shmem_test_wire_pair PRIVATE osoe_base rt)
osoe_warnings(shmem_test_wire_pair)
add_test(NAME shmem.wire-pair
  COMMAND ${CMAKE_COMMAND} -E env OSOE_WIRE_BYTES=64 OSOE_WIRE_SPLIT=3000
          OSOE_WIRE_SEED=20260919
          $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 --
          $<TARGET_FILE:shmem_test_wire_pair>)
set_tests_properties(shmem.wire-pair PROPERTIES LABELS "tier2" TIMEOUT 90)

# Multi-process tests (tier 2): each program runs under osoe-launch at every
# listed local PE count, as shmem.<name>.L<count>.
set(OSOE_LAUNCH_COUNTS 1 2 4)

# The internode phase of a barrier (tier 2). Three nodes at least: with two,
# the root's own arrival and the single message it receives are the first and
# the last, so releasing on any arrival is indistinguishable from releasing on
# all of them - and that mutation passed a two-node test.
add_executable(shmem_test_barrier_am test-barrier-am.c
  ../src/progress.c ../src/am.c ../src/connection.c ../src/connection-area.c
  ../src/reply.c ../src/wire.c ../src/bootstrap.c)
target_include_directories(shmem_test_barrier_am PRIVATE ../src)
target_link_libraries(shmem_test_barrier_am PRIVATE osoe_base rt)
osoe_warnings(shmem_test_barrier_am)
foreach(map 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.barrier-am.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_barrier_am>)
  set_tests_properties(shmem.barrier-am.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# The job-wide rendezvous and the locality function (tier 2), over §8.1's four
# poles: one node, one PE per node, uneven sizes, and ranks not contiguous per
# node - the last two being where two-level collectives break, and a uniform
# map never shows it.
add_executable(shmem_test_job test-job.c ../src/job.c ../src/bootstrap.c
  ../src/am.c)
target_include_directories(shmem_test_job PRIVATE ../src)
target_link_libraries(shmem_test_job PRIVATE osoe_base rt)
osoe_warnings(shmem_test_job)
foreach(map 0 0,0,0,0 0,1,2,3 0,0,0,1,2,2 0,1,2,0,1,0)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.job.${label}
    COMMAND osoe-launch -n ${count} -m ${map} -t 60 -- $<TARGET_FILE:shmem_test_job>)
  set_tests_properties(shmem.job.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# The progress loop between two logical nodes (tier 2): every layer at once.
# The wire is far smaller than a segment and its split injection is on, so the
# stream really arrives in pieces (§8.2). Sizes are pinned rather than
# inherited, so the production defaults cannot make the suite slow or the test
# vacuous.
add_executable(shmem_test_progress test-progress.c
  ../src/progress.c ../src/am.c ../src/connection.c ../src/connection-area.c
  ../src/reply.c ../src/wire.c)
target_include_directories(shmem_test_progress PRIVATE ../src)
target_link_libraries(shmem_test_progress PRIVATE osoe_base rt)
osoe_warnings(shmem_test_progress)
add_test(NAME shmem.progress
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
          OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
          $<TARGET_FILE:shmem_test_progress>)
set_tests_properties(shmem.progress PROPERTIES LABELS "tier2" TIMEOUT 90)

# The connection area across the PEs of a node (tier 2). The local lead creates
# it and the others open it, which is §3.12's arrangement. Not an
# osoe_tier2_test: it does not call shmem_init, testing the area on its own
# below the library, and it takes its identity from the launcher environment.
add_executable(shmem_test_connection_area test-connection-area.c ../src/connection-area.c ../src/connection.c)
target_include_directories(shmem_test_connection_area PRIVATE ../src)
target_link_libraries(shmem_test_connection_area PRIVATE osoe_base rt)
osoe_warnings(shmem_test_connection_area)
foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
  add_test(NAME shmem.connection-area.L${count}
    COMMAND osoe-launch -n ${count} -t 60 -- $<TARGET_FILE:shmem_test_connection_area>)
  set_tests_properties(shmem.connection-area.L${count} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# A job spanning logical nodes, end to end through the public API (tier 2).
#
# Three nodes at least, and more than one PE on a node: with two nodes a root
# that releases on any arrival is indistinguishable from one that waits for all,
# and with one PE per node the intranode phase never runs, so an arrival target
# computed from the wrong member count is right by accident. The maps below have
# the shape that discriminates as well as §8.1's four poles, and one single-node
# map so the same program also covers the path where the last local arriver
# releases the epoch itself.
#
# The AM sizes are pinned small and the wire's split injection is on, so the
# stream really arrives in pieces (§8.2) rather than at whatever the production
# defaults make of loopback.
add_executable(shmem_test_multinode test-multinode.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_multinode PRIVATE
  ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_multinode PRIVATE osoe rt)
osoe_warnings(shmem_test_multinode)
foreach(map 0,0,0,0 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.multinode.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_multinode>)
  set_tests_properties(shmem.multinode.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# The same program with §8.1's force-remote flag on, which is what makes
# osoe_transport_direct's call to the one locality function testable at all:
# without it a PE on another node already has no mapped heap here, so the
# answer is NULL whether the function is consulted or not, and the line that
# matters is the one that returns NULL for a PE which *is* on this node.
#
# The data path does not route yet, so this is the shmem_ptr half of the
# differential test rather than the whole of it. Multi-node maps only: with one
# logical node there is no peer to route to, and what force-remote should mean
# there is an open question (§8.1).
foreach(map 0,1,2 0,0,1,1,2,2)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.multinode.force-remote.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1
            OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_multinode>)
  set_tests_properties(shmem.multinode.force-remote.${label}
    PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# The data path across a node boundary (tier 2): the same program over the
# intranode path and the force-remote path, which must agree byte for byte.
# Sizes are pinned so that a put longer than an AM chunk really is split.
add_executable(shmem_test_internode test-internode.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_internode PRIVATE
  ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_internode PRIVATE osoe rt)
osoe_warnings(shmem_test_internode)
#
# **The multi-node maps do not run under ThreadSanitizer**, and §8.2 already
# says why in general: its view is one process. Here the ordering that makes a
# put safe runs through *another* one. The lead's agent writes a local PE's heap
# while that PE's own main thread touches the same bytes, and what separates
# them is the barrier - this PE's arrival, then whichever local member happened
# to be last posting the handoff, then the root, then the release. Every link is
# a seq_cst operation on shared memory, and the middle ones are in processes
# tsan cannot see, so it reports a race it has no way to disprove.
#
# Suppressing it by name would hide real races in the parser, and adding loads
# for the tool to follow would make the tool green without making anything
# safer. The single-node map still runs everywhere, and there the intranode path
# applies every put in the calling thread, which is precisely the case tsan can
# check. shmem.multinode.* and shmem.barrier-am.* also still run under tsan:
# they cross nodes without an agent ever writing a heap a local thread holds.
if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
  set(osoe_internode_maps 0,0,0,0)
else()
  set(osoe_internode_maps 0,0,0,0 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
endif()
foreach(map IN LISTS osoe_internode_maps)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.internode.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_AM_CREDITS=2
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_internode>)
  set_tests_properties(shmem.internode.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()
# The force-remote half of this test is NOT registered yet, and the reason is an
# open decision rather than an oversight: §8.1's flag makes a PE on *this* node
# remote, and there is no connection from a node to itself (§3.4), so the put
# has nowhere to be routed. shmem.multinode.force-remote.* still runs, because
# it exercises the locality answer rather than the data path.

# Every tier-2 test asserts §8.2's rule that invariant counters are zero at
# quiescence (invariants.h). It reads the control segment through osoe-stat's
# segment reader, compiled in: shared with the tool, deliberately not with the
# library, since a check that used the library's own headers would only prove
# the library agrees with itself.
function(osoe_tier2_test name)
  add_executable(shmem_test_${name} test-${name}.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_${name} PRIVATE osoe rt)
  osoe_warnings(shmem_test_${name})
  foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
    add_test(NAME shmem.${name}.L${count}
      COMMAND osoe-launch -n ${count} -t 60 -- $<TARGET_FILE:shmem_test_${name}>)
    set_tests_properties(shmem.${name}.L${count} PROPERTIES LABELS "tier2" TIMEOUT 90)
  endforeach()
endfunction()

osoe_tier2_test(init)
osoe_tier2_test(fork)
osoe_tier2_test(barrier)
osoe_tier2_test(malloc)
osoe_tier2_test(set-range)
osoe_tier2_test(rma)
osoe_tier2_test(amo)
osoe_tier2_test(wait)
osoe_tier2_test(signal)
osoe_tier2_test(quiet)
osoe_tier2_test(heap)
osoe_tier2_test(landing)
osoe_tier2_test(amo-types)

# The multiple-element waits (§9.11.2-§9.11.14). It reads the waiter's published
# §4.9 watch word to check the unmasked-span refinement, which is the one place
# a test needs the encoding itself rather than a name in the metric table.
# The profiling interface (§10): the test program interposes three routines,
# which only works because the library defines them under pshmem_ names with
# shmem_ as a weak alias.
osoe_tier2_test(surface)

# start_pes leaves no shmem_finalize behind on purpose: what proves the implicit
# one ran is that osoe-launch reports no leaked shared memory (Annex F.2.3).
osoe_tier2_test(start-pes)

osoe_tier2_test(profile)
osoe_tier2_test(team)
osoe_tier2_test(thread)
osoe_tier2_test(wait-many)
target_include_directories(shmem_test_wait-many PRIVATE ../src)

# osoe-stat, against a live job and against one deliberately stalled (§3.14).
# It needs the tool's path, which only the build knows; the launcher passes its
# environment through, so setting it on the test reaches every rank.
osoe_tier2_test(stat)
foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
  set_tests_properties(shmem.stat.L${count} PROPERTIES
    ENVIRONMENT "OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>")
endforeach()

# The development watchdog (§3.15). Not an osoe_tier2_test: it needs -k, so the
# launcher leaves the survivors to the library, and it asserts on output rather
# than on an exit status. A PASS_REGULAR_EXPRESSION test ignores the exit code,
# which is what these need: the job is meant to fail.
add_executable(shmem_test_watchdog test-watchdog.c)
target_link_libraries(shmem_test_watchdog PRIVATE osoe)
osoe_warnings(shmem_test_watchdog)

foreach(count 2 4)
  foreach(where barrier wait_until)
    add_test(NAME shmem.watchdog.${where}.L${count}
      COMMAND osoe-launch -n ${count} -t 20 -k
              -e 0:OSOE_TEST_WAIT=${where} -e 1:OSOE_TEST_WAIT=${where}
              -e 2:OSOE_TEST_WAIT=${where} -e 3:OSOE_TEST_WAIT=${where}
              -e 0:OSOE_WATCHDOG=1 -e 1:OSOE_WATCHDOG=1
              -e 2:OSOE_WATCHDOG=1 -e 3:OSOE_WATCHDOG=1
              -- $<TARGET_FILE:shmem_test_watchdog>)
    set_tests_properties(shmem.watchdog.${where}.L${count} PROPERTIES
      LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "has died; this PE would wait for it forever")
  endforeach()
endforeach()

# Turned off, the same job hangs until the launcher's timeout: this is what the
# watchdog replaces, kept as a test so the difference stays demonstrated.
add_test(NAME shmem.watchdog.off
  COMMAND osoe-launch -n 2 -t 3 -k -e 0:OSOE_WATCHDOG=0 -e 1:OSOE_WATCHDOG=0
          -- $<TARGET_FILE:shmem_test_watchdog>)
set_tests_properties(shmem.watchdog.off PROPERTIES
  LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe-launch: timeout after 3 s")

# With nothing set, the default must follow the preset: on in debug, asan and
# tsan, off in release (§3.15).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(osoe_watchdog_default_expect "has died; this PE would wait for it forever")
else()
  set(osoe_watchdog_default_expect "osoe-launch: timeout after 3 s")
endif()
add_test(NAME shmem.watchdog.default
  COMMAND osoe-launch -n 2 -t 3 -k -- $<TARGET_FILE:shmem_test_watchdog>)
set_tests_properties(shmem.watchdog.default PROPERTIES
  LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "${osoe_watchdog_default_expect}")

# Init must refuse what it cannot run, saying why.
add_test(NAME shmem.refuse.no_launcher
  COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_PROCID --unset=SLURM_JOB_ID
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.no_launcher PROPERTIES
  PASS_REGULAR_EXPRESSION "no launcher environment")
# A multi-node job is no longer refused; shmem.multinode.* is what it does
# instead, and shmem.init.* still covers the one-node case.
add_test(NAME shmem.refuse.bad_size
  COMMAND osoe-launch -n 2 -t 60 -e 0:SHMEM_SYMMETRIC_SIZE=12q -e 1:SHMEM_SYMMETRIC_SIZE=12q
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.bad_size PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE='12q' is invalid")
add_test(NAME shmem.refuse.size_mismatch
  COMMAND osoe-launch -n 3 -t 60 -e 2:SHMEM_SYMMETRIC_SIZE=2m
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.size_mismatch PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE differs between PEs")
# Destroying a predefined team is undefined by spec §9.4.8, and undefined is
# loud here: no other team can exist, so every call naming one is a program bug.
add_executable(shmem_test_bad_team test-bad-team.c)
target_link_libraries(shmem_test_bad_team PRIVATE osoe)
osoe_warnings(shmem_test_bad_team)
add_test(NAME shmem.refuse.destroy_predefined_team
  COMMAND osoe-launch -n 1 -t 60 -- $<TARGET_FILE:shmem_test_bad_team>)
set_tests_properties(shmem.refuse.destroy_predefined_team PROPERTIES
  LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "SHMEM_TEAM_WORLD is a predefined team and cannot be destroyed")

add_executable(shmem_test_bad_put test-bad-put.c)
target_link_libraries(shmem_test_bad_put PRIVATE osoe)
osoe_warnings(shmem_test_bad_put)
add_test(NAME shmem.refuse.nonsymmetric_put
  COMMAND osoe-launch -n 1 -t 60 -- $<TARGET_FILE:shmem_test_bad_put>)
set_tests_properties(shmem.refuse.nonsymmetric_put PROPERTIES
  PASS_REGULAR_EXPRESSION "shmem_putmem: .* is not within the symmetric heap")
set_tests_properties(shmem.refuse.no_launcher shmem.refuse.bad_size
  shmem.refuse.size_mismatch shmem.refuse.nonsymmetric_put PROPERTIES
  LABELS "tier2" TIMEOUT 90)
