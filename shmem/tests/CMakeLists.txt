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
    "length|runs past the end of segment 1 from an unnamed peer"
    "short-segment|less than its own header"
    "kind|this build knows"
    "short-message|less than its fixed part"
    "long-message|runs past the end of segment"
    "empty-count|messages in no room")
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
add_executable(shmem_test_am_size test-am-size.c ../src/connection-area.c ../src/connection.c
  ../src/shm-object.c)
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
add_executable(shmem_test_wire test-wire.c ../src/wire.c ../src/shm-object.c)
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
add_executable(shmem_test_wire_pair test-wire-pair.c ../src/wire.c ../src/shm-object.c)
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
  ../src/reply.c ../src/wire.c ../src/bootstrap.c ../src/shm-object.c)
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
add_executable(shmem_test_job test-job.c ../src/job.c ../src/bootstrap.c ../src/shm-object.c
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
# Two node leads claiming one rank: node 1's lead is told its node holds ranks 1
# and 0, and node 0's lead holds 0. Exactly one claim wins (a CAS per rank since
# slice 2; a plain test-and-store before, which two simultaneous leads could
# both pass), and the job is refused naming the rank.
add_test(NAME shmem.job.duplicate-rank
  COMMAND osoe-launch -n 3 -m 0,1,1 -t 30 -e 1:OSOE_GTIDS=1,0 --
          $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.duplicate-rank PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "rank 0 is claimed by node [01] and by node [01]")

# The progress loop between two logical nodes (tier 2): every layer at once.
# The wire is far smaller than a segment and its split injection is on, so the
# stream really arrives in pieces (§8.2). Sizes are pinned rather than
# inherited, so the production defaults cannot make the suite slow or the test
# vacuous.
add_executable(shmem_test_progress test-progress.c
  ../src/progress.c ../src/am.c ../src/connection.c ../src/connection-area.c
  ../src/reply.c ../src/wire.c ../src/shm-object.c)
target_include_directories(shmem_test_progress PRIVATE ../src)
target_link_libraries(shmem_test_progress PRIVATE osoe_base rt)
osoe_warnings(shmem_test_progress)
add_test(NAME shmem.progress
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
          OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
          $<TARGET_FILE:shmem_test_progress>)
set_tests_properties(shmem.progress PROPERTIES LABELS "tier2" TIMEOUT 90)
# Review round two, D4: a get reply nobody asked for, pointing past the landing
# zone, must be refused by name on the node that receives it. Before slice 2 it
# was written, 64 bytes past the zone, and the test passed.
add_test(NAME shmem.progress.forged-get-reply
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
          OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
          OSOE_TEST_FORGE=get-reply
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 20 --
          $<TARGET_FILE:shmem_test_progress>)
set_tests_properties(shmem.progress.forged-get-reply PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe_fatal: a get reply from node 0"
  FAIL_REGULAR_EXPRESSION "WROTE PAST THE LANDING ZONE")
add_test(NAME shmem.progress.forged-get-reply-bounds
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
          OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
          OSOE_TEST_FORGE=get-reply-bounds
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 20 --
          $<TARGET_FILE:shmem_test_progress>)
set_tests_properties(shmem.progress.forged-get-reply-bounds PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe_fatal: a get reply from node 0 for PE 2 puts 64 bytes at offset"
  FAIL_REGULAR_EXPRESSION "WROTE PAST THE LANDING ZONE")

# The connection area across the PEs of a node (tier 2). The local lead creates
# it and the others open it, which is §3.12's arrangement. Not an
# osoe_tier2_test: it does not call shmem_init, testing the area on its own
# below the library, and it takes its identity from the launcher environment.
add_executable(shmem_test_connection_area test-connection-area.c ../src/connection-area.c ../src/connection.c
  ../src/shm-object.c)
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

# The differential test (§8.1): one program, §8.1's four poles of the logical
# node map at one world size, and the transcripts must match. On the first map
# every operation is applied in the calling thread; on the second every one
# crosses the AM layer; the middle two mix them, which is where a path right at
# both extremes and wrong in between shows up.
#
# It compares outputs rather than checking assertions, which is why it is worth
# having beside shmem.internode: a CHECK catches what it was told to look for,
# a diff catches anything that differs.
#
# Not registered under ThreadSanitizer, for the reason given above: the
# happens-before that makes an applied put safe runs through another process,
# and tsan sees one. Reducing it to the single-node map, as shmem.internode is
# reduced, would leave nothing to compare it against - a differential test of
# one configuration is not one - so it is left out there entirely and the other
# three presets carry it.
add_executable(shmem_test_differential test-differential.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_differential PRIVATE
  ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_differential PRIVATE osoe rt)
osoe_warnings(shmem_test_differential)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
add_test(NAME shmem.differential
  COMMAND ${CMAKE_COMMAND}
          -DLAUNCH=$<TARGET_FILE:osoe-launch>
          -DEXE=$<TARGET_FILE:shmem_test_differential>
          -DNRANKS=6
          "-DMAPS=0,0,0,0,0,0;0,1,2,3,4,5;0,0,0,1,2,2;0,1,2,0,1,0"
          "-DENVS=OSOE_AM_CHUNK=1024;OSOE_AM_BUFFER=2048;OSOE_AM_CREDITS=2;OSOE_WIRE_BYTES=256;OSOE_WIRE_SPLIT=2000;OSOE_WIRE_SEED=20260919;SHMEM_SYMMETRIC_SIZE=16m"
          -P ${PROJECT_SOURCE_DIR}/cmake/Differential.cmake)
set_tests_properties(shmem.differential PROPERTIES LABELS "tier2" TIMEOUT 300)
endif()

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

# A tier-2 test over a logical node map (§8.1) rather than a PE count: for
# what only exists between nodes. Built like osoe_tier2_test, with the segment
# reader, so it can read the control segment's metrics by name.
function(osoe_tier2_map_test name map)
  if(NOT TARGET shmem_test_${name})
    add_executable(shmem_test_${name} test-${name}.c
      ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
    target_include_directories(shmem_test_${name} PRIVATE
      ${PROJECT_SOURCE_DIR}/tools/stat)
    target_link_libraries(shmem_test_${name} PRIVATE osoe rt)
    osoe_warnings(shmem_test_${name})
  endif()
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.${name}.${label}
    COMMAND ${CMAKE_COMMAND} -E env ${ARGN}
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_${name}>)
  set_tests_properties(shmem.${name}.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
endfunction()

# A quiesced pair falls silent (§3.4; review round two, C1). Before slice 2's
# acknowledgement protocol: 1,320 header-only segments per node in the idle
# half second.
osoe_tier2_map_test(ack-echo 0,1)

# A reply is not starved by puts (§3.8's DRAINING state; review round two, C7):
# one getter on node 0, the get's target and five writers on node 1, the
# smallest connection §3.4 allows, puts of 984 bytes - two tile the buffer and
# leave no room for a reply. A guard, not a reproduction: it passed before the
# pre-open drain too (§3.8 says why), so it holds the line against a real hang.
#
# These seven-PE tests size their heaps to what they use (16 MiB): at the
# default 256 MiB, six or seven PEs on one node ask for more than a 3 GiB
# machine's /dev/shm, which §6.2's capacity check refuses at init.
#
# Neither this nor shmem.reply-pool below runs under ThreadSanitizer, for the
# reason shmem.internode gives: an agent serves a get from, or applies a put to,
# a heap the local main thread wrote before a barrier, and that barrier's
# ordering runs through processes tsan cannot see. Both reported exactly that
# race, and only that, when slice 2 first ran them under tsan.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(reply-starvation 0,1,1,1,1,1,1
    OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_WIRE_BYTES=4096 OSOE_TEST_PUT_BYTES=984
    SHMEM_SYMMETRIC_SIZE=16m)
endif()

# An internode quiet sweeps (§4.10; review round two, D2): node 1 is stopped,
# PE 0 quiets toward it, PE 1 sees it blocked in osoe-stat and dies, and PE 0's
# next sweep must end the job naming shmem_quiet. -k leaves the survivors to the
# library. Before slice 2 the quiet never swept: osoe-stat showed nothing for
# PE 0, and the job ran to the launcher's 15 s timeout.
add_executable(shmem_test_unswept-wait test-unswept-wait.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_unswept-wait PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_unswept-wait PRIVATE osoe rt)
osoe_warnings(shmem_test_unswept-wait)
add_test(NAME shmem.unswept-wait
  COMMAND ${CMAKE_COMMAND} -E env OSOE_WATCHDOG=1 OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
          $<TARGET_FILE:osoe-launch> -n 3 -m 0,0,1 -t 15 -k --
          $<TARGET_FILE:shmem_test_unswept-wait>)
set_tests_properties(shmem.unswept-wait PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_quiet: local PE 1 [(]rank 1"
  FAIL_REGULAR_EXPRESSION "BLOCKED RECORD WRONG|QUIET RETURNED")

# The reply descriptor pool (§3.8; review round two, D1). The review's scenario
# at C = 2 must complete; with the target given a smaller credit than the
# origin, the pool overflows and the target must say so by name. Before slice 2
# sized the pool by §3.8's formula, both dropped a get request without a word
# and timed out ("no get reply after 60 s"). The completing half is not run
# under ThreadSanitizer (see shmem.reply-starvation); the overflow half is,
# since its pass is the named fatal and not a clean exit.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(reply-pool 0,0,0,1 OSOE_AM_CREDITS=2 SHMEM_SYMMETRIC_SIZE=16m)
else()
  add_executable(shmem_test_reply-pool test-reply-pool.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_reply-pool PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_reply-pool PRIVATE osoe rt)
  osoe_warnings(shmem_test_reply-pool)
endif()
add_test(NAME shmem.reply-pool.overflow
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_POOL=overflow SHMEM_SYMMETRIC_SIZE=16m
          $<TARGET_FILE:osoe-launch> -n 8 -m 0,0,0,0,0,0,0,1 -t 30
          -e 7:OSOE_AM_CREDITS=1 -- $<TARGET_FILE:shmem_test_reply-pool>)
set_tests_properties(shmem.reply-pool.overflow PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "reply descriptor pool is empty.*request from node 0")

# A quiet waits as long as its peer does (§3.15; review round two, D3): node 1
# stopped for 3 s, then continued, and the quiet completes with the no-progress
# limit unset; with it at 1 s, the quiet ends the job naming the connection.
# The slice-2 commit records a run stopped for 65 s, past the old 60 s bound.
osoe_tier2_map_test(stopped-node 0,0,1 OSOE_TEST_STOP_MS=3000)
set_tests_properties(shmem.stopped-node.0-0-1 PROPERTIES
  PASS_REGULAR_EXPRESSION "quiet completed after node 1 was stopped")
add_test(NAME shmem.stopped-node.no-progress
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STOP_MS=6000 OSOE_NO_PROGRESS_TIMEOUT=1
          $<TARGET_FILE:osoe-launch> -n 3 -m 0,0,1 -t 30 --
          $<TARGET_FILE:shmem_test_stopped-node>)
set_tests_properties(shmem.stopped-node.no-progress PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_quiet: no progress toward node 1 for [0-9]+ s"
  FAIL_REGULAR_EXPRESSION "quiet completed")

# A loud failure ends the job without joining a collective (§3.15; review round
# two, conformance F1): osoe_fatal leaves through _exit, so start_pes's atexit
# finalize - a barrier - does not run. Before slice 2 this job hung 7-8 s.
add_executable(shmem_test_fatal-start-pes test-fatal-start-pes.c)
target_link_libraries(shmem_test_fatal-start-pes PRIVATE osoe)
osoe_warnings(shmem_test_fatal-start-pes)
add_test(NAME shmem.fatal-start-pes
  COMMAND osoe-launch -n 3 -t 10 -- $<TARGET_FILE:shmem_test_fatal-start-pes>)
set_tests_properties(shmem.fatal-start-pes PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_putmem: .* is not within the symmetric heap"
  FAIL_REGULAR_EXPRESSION "timeout after|RETURNED")

# A PE leaving without shmem_finalize (§6.4; review round two, D14). See the
# test's header for the two modes and what each must and must not do.
add_executable(shmem_test_exit-without-finalize test-exit-without-finalize.c)
target_link_libraries(shmem_test_exit-without-finalize PRIVATE osoe)
osoe_warnings(shmem_test_exit-without-finalize)
add_test(NAME shmem.exit-without-finalize.init
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MODE=init
          $<TARGET_FILE:osoe-launch> -n 3 -t 10 -k --
          $<TARGET_FILE:shmem_test_exit-without-finalize>)
set_tests_properties(shmem.exit-without-finalize.init PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "PE 1: the process exited without calling shmem_finalize"
  FAIL_REGULAR_EXPRESSION "timeout after|BARRIER RETURNED")
add_test(NAME shmem.exit-without-finalize.start_pes
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MODE=start_pes
          $<TARGET_FILE:osoe-launch> -n 3 -t 4 --
          $<TARGET_FILE:shmem_test_exit-without-finalize>)
set_tests_properties(shmem.exit-without-finalize.start_pes PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe-launch: timeout after 4 s"
  FAIL_REGULAR_EXPRESSION "BARRIER RETURNED")

# No death of any process strands a named object (§3.14; review round two,
# D13): every PE dies of SIGKILL or SIGTERM after init, on two nodes, and the
# launcher must find nothing left behind. The kept variant leaves the control
# segments on purpose, and osoe-stat --gc must then remove them.
add_executable(shmem_test_kill-after-init test-kill-after-init.c)
target_link_libraries(shmem_test_kill-after-init PRIVATE osoe)
osoe_warnings(shmem_test_kill-after-init)
foreach(sig KILL TERM)
  add_test(NAME shmem.kill-after-init.${sig}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_SIGNAL=${sig}
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -k --
            $<TARGET_FILE:shmem_test_kill-after-init>)
  set_tests_properties(shmem.kill-after-init.${sig} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "killed by signal"
    FAIL_REGULAR_EXPRESSION "left behind")
endforeach()
add_test(NAME shmem.kill-after-init.kept
  COMMAND sh -c "OSOE_KEEP_CONTROL_SEGMENT=1 OSOE_TEST_SIGNAL=KILL \"$0\" -n 4 -m 0,0,1,1 -t 30 -k -- \"$1\"; \"$2\" --gc"
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_kill-after-init>
          $<TARGET_FILE:osoe-stat>)
set_tests_properties(shmem.kill-after-init.kept PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "kept for post-mortem reading.*removed"
  FAIL_REGULAR_EXPRESSION "left behind")

# Init reclaims leftovers of a job killed during init, and refuses an object
# whose creator is alive (§6.2; review round two, D11, D15). See leftovers.sh.
add_test(NAME shmem.leftovers.reclaim
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/leftovers.sh $<TARGET_FILE:osoe-launch>
          $<TARGET_FILE:shmem_test_init> reclaim)
set_tests_properties(shmem.leftovers.reclaim PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "reclaimed /dev/shm.*job exited 0.*with a name afterwards: 0")
add_test(NAME shmem.leftovers.live
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/leftovers.sh $<TARGET_FILE:osoe-launch>
          $<TARGET_FILE:shmem_test_init> live)
set_tests_properties(shmem.leftovers.live PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "the refusal names the owner: yes")

# A job never outlives its launcher: SIGTERM and SIGINT are forwarded, and a
# SIGKILLed launcher takes its PEs with it (launch.c; review round two,
# omissions 6). See launcher-signals.sh.
add_executable(shmem_test_sleeper sleeper.c)
target_link_libraries(shmem_test_sleeper PRIVATE osoe)
osoe_warnings(shmem_test_sleeper)
foreach(mode term int kill)
  add_test(NAME shmem.launcher-signals.${mode}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/launcher-signals.sh
            $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_sleeper> ${mode})
  set_tests_properties(shmem.launcher-signals.${mode} PROPERTIES LABELS "tier2"
    TIMEOUT 60)
endforeach()
set_tests_properties(shmem.launcher-signals.term PROPERTIES PASS_REGULAR_EXPRESSION
  "launcher exited 143.*escalated to SIGKILL: 0.*PEs still running: 0.*with a name: 0")
set_tests_properties(shmem.launcher-signals.int PROPERTIES PASS_REGULAR_EXPRESSION
  "launcher exited 130.*escalated to SIGKILL: 0.*PEs still running: 0.*with a name: 0")
set_tests_properties(shmem.launcher-signals.kill PROPERTIES PASS_REGULAR_EXPRESSION
  "PEs still running: 0.*with a name: 0")

# What a core holds: this PE's heap, not its siblings' or the connection area
# (review round two, omissions 4).
osoe_tier2_map_test(core-exclusion 0,0,1,1)

# A request larger than /dev/shm is refused at init, by name (§6.2; review round
# two, omissions 5). See dev-shm-capacity.sh.
add_test(NAME shmem.dev-shm-capacity.heaps
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> heaps)
add_test(NAME shmem.dev-shm-capacity.connections
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> connections)
set_tests_properties(shmem.dev-shm-capacity.heaps shmem.dev-shm-capacity.connections
  PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.dev-shm-capacity.heaps PROPERTIES PASS_REGULAR_EXPRESSION
  "/dev/shm has [0-9]+ MiB free, but this node's control segment and 2 heaps.*launcher exited 1")
set_tests_properties(shmem.dev-shm-capacity.connections PROPERTIES PASS_REGULAR_EXPRESSION
  "/dev/shm has [0-9]+ MiB free, but this node's connection area.*launcher exited 1")

# A kernel without futex2 (Linux < 6.7) is refused at init by name, before any
# object exists (§1; review round two, portability F3). deny-futex2 makes the
# three calls fail with ENOSYS, as such a kernel does. Before, both PEs died of
# SIGABRT in the rendezvous and left their heaps and control segment behind.
# And the probe, under the same filter, fails its named-object futex checks.
add_executable(deny-futex2 deny-futex2.c)
osoe_warnings(deny-futex2)
add_test(NAME shmem.refuse.old-kernel
  COMMAND deny-futex2 $<TARGET_FILE:osoe-launch> -n 2 -t 30 --
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.old-kernel PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "needs Linux 6.7 or later, for the futex2 calls .*this kernel is"
  FAIL_REGULAR_EXPRESSION "left behind|futex_wake failed|futex_wait failed")
add_test(NAME env.probe.old-kernel COMMAND deny-futex2 $<TARGET_FILE:probe>)
set_tests_properties(env.probe.old-kernel PROPERTIES LABELS "env" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "FAIL futex_wait, named shm, woken by other process")

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
      PASS_REGULAR_EXPRESSION "has died[;] this PE would wait for it forever")
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
  set(osoe_watchdog_default_expect "has died[;] this PE would wait for it forever")
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
