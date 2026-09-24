add_executable(shmem_test_info test-info.c)
target_link_libraries(shmem_test_info PRIVATE osoe)
osoe_warnings(shmem_test_info)
add_test(NAME shmem.info COMMAND shmem_test_info)
set_tests_properties(shmem.info PROPERTIES LABELS "tier1" TIMEOUT 30)

# shmem_pcontrol declared by <shmem.h> alone (F7), and <mpp/shmemx.h> (F8):
# both are proven by compiling, not by a runtime check. See the file comment.
add_executable(shmem_test_header_surface test-header-surface.c)
target_link_libraries(shmem_test_header_surface PRIVATE osoe)
osoe_warnings(shmem_test_header_surface)
add_test(NAME shmem.header-surface COMMAND shmem_test_header_surface)
set_tests_properties(shmem.header-surface PROPERTIES LABELS "tier1" TIMEOUT 30)

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

# The backend boundary (§4.10): no file above transport.h and internode.h may
# include, however indirectly, a header of the backend below. The two sides
# are defined as lists in the script, and adding `#include "progress.h"` to
# rma.c is the case it was written to fail (second review, roce-seam F6).
add_test(NAME shmem.boundary
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckBoundary.cmake)
set_tests_properties(shmem.boundary PROPERTIES LABELS "tier1" TIMEOUT 30)

# A static build is not one of the four presets (none sets BUILD_SHARED_LIBS),
# so this proves it on its own: a nested configure and build of libosoe as a
# static archive, then a link of a tiny consumer against that archive alone -
# never against osoe_base, which is never installed. Portability review F10:
# before osoe_base became an OBJECT library, this failed with over a hundred
# undefined references.
add_test(NAME shmem.static-link
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/static-link-check
          -DPROBE_SRC=${CMAKE_CURRENT_SOURCE_DIR}/static-link-probe.c
          -DC_COMPILER=${CMAKE_C_COMPILER}
          "-DURING_LIBRARIES=${LIBURING_LINK_LIBRARIES}"
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckStaticLink.cmake)
set_tests_properties(shmem.static-link PROPERTIES LABELS "tier1" TIMEOUT 180)

# None of the four presets sets CMAKE_C_EXTENSIONS, so plain -std=c11 (design
# §1: "a C11 compiler") is otherwise never built. A nested configure and
# build, the same shape as shmem.static-link above. Portability review F12.
add_test(NAME shmem.strict-c11
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/strict-c11-check
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckStrictC11.cmake)
set_tests_properties(shmem.strict-c11 PROPERTIES LABELS "tier1" TIMEOUT 180)

# The allocator on its own (tier 1): compiled in, since libosoe hides it.
add_executable(shmem_test_allocator test-allocator.c ../src/allocator.c)
target_include_directories(shmem_test_allocator PRIVATE ../src)
osoe_warnings(shmem_test_allocator)
add_test(NAME shmem.allocator COMMAND shmem_test_allocator)
set_tests_properties(shmem.allocator PROPERTIES LABELS "tier1" TIMEOUT 60)

# The internode tree's topology on its own (tier 1; §5.3): sparse node lists,
# every fan-out a test uses, and the depths the tree must have.
add_executable(shmem_test_collective_tree test-collective-tree.c ../src/collective-tree.c)
target_include_directories(shmem_test_collective_tree PRIVATE ../src)
osoe_warnings(shmem_test_collective_tree)
add_test(NAME shmem.collective-tree COMMAND shmem_test_collective_tree)
set_tests_properties(shmem.collective-tree PROPERTIES LABELS "tier1" TIMEOUT 60)

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

# The compressed rank map on its own (tier 1): progressions, runs, lookups against
# the uncompressed map, the refusals, and the sizes at 4,096 x 256 (§6.2).
add_executable(shmem_test_rank-map test-rank-map.c ../src/rank-map.c)
target_include_directories(shmem_test_rank-map PRIVATE ../src)
osoe_warnings(shmem_test_rank-map)
add_test(NAME shmem.rank-map COMMAND shmem_test_rank-map)
set_tests_properties(shmem.rank-map PROPERTIES LABELS "tier1" TIMEOUT 60)

# The node-pair wire on its own (tier 1): both ends in one process, so no
# launcher. wire.c is compiled in, since libosoe hides it, and the test supplies
# its own osoe_fatal so a failure inside it fails the test.
add_executable(shmem_test_wire test-wire.c ../src/wire.c ../src/ring.c ../src/listener.c
  ../src/shm-object.c)
target_include_directories(shmem_test_wire PRIVATE ../src)
target_link_libraries(shmem_test_wire PRIVATE osoe_base rt PkgConfig::LIBURING)
osoe_warnings(shmem_test_wire)
add_test(NAME shmem.wire COMMAND shmem_test_wire)
set_tests_properties(shmem.wire PROPERTIES LABELS "tier1" TIMEOUT 120)

# The wire between two processes (tier 2). Two logical nodes, one PE each, so
# each process is a node lead - the only role that touches a wire (§3.12). It
# does not call shmem_init: it tests the wire on its own, below the library,
# and takes its identity straight from the launcher environment.
# The seed is fixed: §3.14 wants an injected fault reproducible, and without it
# the pattern would follow the launcher's pid and differ every run.
add_executable(shmem_test_wire_pair test-wire-pair.c ../src/wire.c ../src/ring.c
  ../src/listener.c ../src/shm-object.c)
target_include_directories(shmem_test_wire_pair PRIVATE ../src)
target_link_libraries(shmem_test_wire_pair PRIVATE osoe_base rt PkgConfig::LIBURING)
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
  ../src/progress.c ../src/collective-tree.c ../src/am.c ../src/connection.c ../src/connection-area.c
  ../src/reply.c ../src/wire.c ../src/ring.c ../src/listener.c ../src/bootstrap.c
  ../src/shm-object.c)
target_include_directories(shmem_test_barrier_am PRIVATE ../src)
target_link_libraries(shmem_test_barrier_am PRIVATE osoe_base rt PkgConfig::LIBURING)
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
# A loud failure writes its reason before it publishes anything (fatal.h),
# checked on fatal.c alone. See test-fatal-order.c.
add_executable(shmem_test_fatal-order test-fatal-order.c ../src/fatal.c)
target_include_directories(shmem_test_fatal-order PRIVATE ../src)
target_link_libraries(shmem_test_fatal-order PRIVATE osoe_base)
osoe_warnings(shmem_test_fatal-order)
add_test(NAME shmem.fatal-order COMMAND shmem_test_fatal-order)
set_tests_properties(shmem.fatal-order PROPERTIES LABELS "tier1" TIMEOUT 30)

add_executable(shmem_test_job test-job.c ../src/job.c ../src/bootstrap.c ../src/shm-object.c
  ../src/am.c ../src/rendezvous.c ../src/rank-map.c ../src/listener.c)
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
# and 0, and node 0's lead holds 0. The rendezvous's root checks every node's
# ranks once, as it builds the map (rank-map.c), and the job is refused naming
# the rank and both nodes. (Step 2 had each lead claim its ranks by CAS in a
# job-wide array; a plain test-and-store before slice 2 let two leads both pass.)
add_test(NAME shmem.job.duplicate-rank
  COMMAND osoe-launch -n 3 -m 0,1,1 -t 30 -e 1:OSOE_GTIDS=1,0 --
          $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.duplicate-rank PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "rank 0 is claimed by node [01] and by node [01]")

# A rank no lead claims: node 1's lead is told its node holds rank 1 alone,
# though rank 2 is there too. The root checks the map once as it builds it
# (it was every PE's O(P) scan before slice 3), and the job is refused naming
# the rank.
add_test(NAME shmem.job.unclaimed-rank
  COMMAND osoe-launch -n 3 -m 0,1,1 -t 30 -e 1:OSOE_GTIDS=1 --
          $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.unclaimed-rank PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "rank 2 was not claimed by any node")

# The same refusal with its message's write held back a second (slow-message.c):
# the reason must be on stderr before the job is told to fail, or the launcher's
# SIGTERM can take it. Before the reason was written first, it was lost in 1 of
# 30 runs of the test above under load, and in every run of this one. Not under
# the sanitizers, whose runtimes must come first in the link order LD_PRELOAD
# would change.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=")
  add_library(slow-message SHARED slow-message.c)
  target_link_libraries(slow-message PRIVATE ${CMAKE_DL_LIBS})
  osoe_warnings(slow-message)
  add_test(NAME shmem.job.duplicate-rank.slow-message
    COMMAND ${CMAKE_COMMAND} -E env "OSOE_TEST_SLOW_TEXT=is claimed by node"
            LD_PRELOAD=$<TARGET_FILE:slow-message>
            $<TARGET_FILE:osoe-launch> -n 3 -m 0,1,1 -t 30 -e 1:OSOE_GTIDS=1,0 --
            $<TARGET_FILE:shmem_test_job>)
  set_tests_properties(shmem.job.duplicate-rank.slow-message PROPERTIES LABELS "tier2"
    TIMEOUT 60 PASS_REGULAR_EXPRESSION "rank 0 is claimed by node [01] and by node [01]")
endif()

# The values every node must agree on are checked up the rendezvous tree, each
# by the parent that sees the disagreement, naming both nodes (§6.2). Node 2's
# parent is node 0 at the tree's fan-out of 8.
add_test(NAME shmem.job.disagree.symmetric-size
  COMMAND osoe-launch -n 3 -m 0,1,2 -t 30 -e 2:SHMEM_SYMMETRIC_SIZE=64M --
          $<TARGET_FILE:shmem_test_job>)
add_test(NAME shmem.job.disagree.fanout
  COMMAND osoe-launch -n 3 -m 0,1,2 -t 30 -e 2:OSOE_COLLECTIVE_FANOUT=2 --
          $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.disagree.symmetric-size shmem.job.disagree.fanout
  PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.job.disagree.symmetric-size PROPERTIES PASS_REGULAR_EXPRESSION
  "node 2 [(]or a node below it[)] has SHMEM_SYMMETRIC_SIZE [0-9]+; node 0 has")
set_tests_properties(shmem.job.disagree.fanout PROPERTIES PASS_REGULAR_EXPRESSION
  "node 2 [(]or a node below it[)] has OSOE_COLLECTIVE_FANOUT=2; node 0 has 4")

# The node count is the value that shapes the waits: a lead expects children by
# its own count, so one given too many waits for children that do not exist.
# The hello carries the count and a lead calls its parent before it waits for
# its children, so the first edge that disagrees refuses the job, rather than
# every lead waiting with no message (tools/modelcheck/rendezvous_model.py).
# Node 1 given 10 nodes expects a child 9; node 0 given 9 expects 1 to 8.
add_test(NAME shmem.job.disagree.node-count
  COMMAND osoe-launch -n 3 -m 0,1,2 -t 30 -e 1:OSOE_STEP_NUM_NODES=10
          -e 1:OSOE_STEP_NODELIST=127.0.0.[1-10] -- $<TARGET_FILE:shmem_test_job>)
add_test(NAME shmem.job.disagree.root-node-count
  COMMAND osoe-launch -n 3 -m 0,1,2 -t 30 -e 0:OSOE_STEP_NUM_NODES=9
          -e 0:OSOE_STEP_NODELIST=127.0.0.[1-9] -- $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.disagree.node-count shmem.job.disagree.root-node-count
  PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.job.disagree.node-count PROPERTIES PASS_REGULAR_EXPRESSION
  "node 0, launched for 3 tasks on 3 nodes, was called by node 1, launched for 3 tasks on 10 nodes")
set_tests_properties(shmem.job.disagree.root-node-count PROPERTIES PASS_REGULAR_EXPRESSION
  "node 0, launched for 3 tasks on 9 nodes, was called by node [12], launched for 3 tasks on 3 nodes")

# OSOE_JOB_SECRET (§6.2, §10): leads that share it meet; a lead that has none,
# or a different one, is refused by name - by the proof in the hello, which a
# lead that does not know the secret cannot make.
set(osoe_secret 0123456789abcdef0123456789abcdef)
add_test(NAME shmem.job.secret
  COMMAND ${CMAKE_COMMAND} -E env OSOE_JOB_SECRET=${osoe_secret}
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 30 -- $<TARGET_FILE:shmem_test_job>)
add_test(NAME shmem.job.secret.missing
  COMMAND ${CMAKE_COMMAND} -E env OSOE_JOB_SECRET=${osoe_secret}
          $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 30 -e 1:OSOE_JOB_SECRET= --
          $<TARGET_FILE:shmem_test_job>)
add_test(NAME shmem.job.secret.different
  COMMAND ${CMAKE_COMMAND} -E env OSOE_JOB_SECRET=${osoe_secret}
          $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 30
          -e 1:OSOE_JOB_SECRET=fedcba9876543210fedcba9876543210 --
          $<TARGET_FILE:shmem_test_job>)
set_tests_properties(shmem.job.secret shmem.job.secret.missing shmem.job.secret.different
  PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.job.secret.missing PROPERTIES
  PASS_REGULAR_EXPRESSION "node 0 has an OSOE_JOB_SECRET and node 1 has none")
set_tests_properties(shmem.job.secret.different PROPERTIES
  PASS_REGULAR_EXPRESSION "a proof of a different OSOE_JOB_SECRET")

# The library against its floor (§1, §8.4): in each of seven pairs, 8-byte
# io_uring round trips on a raw connection between the job's two processes
# interleaved with the library's get, put and quiet and fetch-add, then a
# stream on each. The pins (test-floor.c) are checked in the optimized,
# uninstrumented build only; the other presets run the paths and the
# invariants. Not under ThreadSanitizer, for shmem.internode's reason below:
# the lead's agent applies PE 0's puts to a heap whose owner last touched it
# before a barrier that runs through another process.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_executable(shmem_test_floor test-floor.c ${PROJECT_SOURCE_DIR}/base/hostlist.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_floor PRIVATE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_floor PRIVATE osoe rt PkgConfig::LIBURING)
  osoe_warnings(shmem_test_floor)
  add_test(NAME shmem.floor
    COMMAND osoe-launch -n 2 -m 0,1 -t 150 -- $<TARGET_FILE:shmem_test_floor>)
  set_tests_properties(shmem.floor PROPERTIES LABELS "tier2" TIMEOUT 180)
endif()

# The lead's half of the secret (tier 1): a caller whose hello is right in every
# field but who cannot prove the secret - replaying the answer's own proof - is
# refused and not counted, and the lead goes on listening. Only a caller that
# lies reaches this check, so test-listener.c is one, and includes listener.c
# for the hello's layout.
add_executable(shmem_test_listener test-listener.c)
target_include_directories(shmem_test_listener PRIVATE ../src)
target_link_libraries(shmem_test_listener PRIVATE osoe_base Threads::Threads)
osoe_warnings(shmem_test_listener)
add_test(NAME shmem.listener.secret COMMAND shmem_test_listener)
set_tests_properties(shmem.listener.secret PROPERTIES LABELS "tier1" TIMEOUT 60)

# The progress loop between two logical nodes (tier 2): every layer at once.
# The wire is far smaller than a segment and its split injection is on, so the
# stream really arrives in pieces (§8.2). Sizes are pinned rather than
# inherited, so the production defaults cannot make the suite slow or the test
# vacuous.
add_executable(shmem_test_progress test-progress.c
  ../src/progress.c ../src/collective-tree.c ../src/am.c ../src/connection.c ../src/connection-area.c
  ../src/reply.c ../src/wire.c ../src/ring.c ../src/listener.c ../src/shm-object.c)
target_include_directories(shmem_test_progress PRIVATE ../src)
target_link_libraries(shmem_test_progress PRIVATE osoe_base rt PkgConfig::LIBURING)
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

# The internode tree (§5.3), over node counts where its shape matters: 7 and
# 13 nodes leave the last level partly filled, and 8 at k = 2 is depth 3. One
# PE per node, where the tree is all there is, and uneven maps with several
# PEs on some nodes, where the intranode phase and the handoff run under it.
# The same program as shmem.multinode.*: its arrival table (arrivals.h) is what
# says no PE left before the last arrived, and it ends by requiring every
# invariant counter, missed_wake among them, to be zero.
set(osoe_tree_maps
  0,1,2,3,4,5,6
  0,1,2,3,4,5,6,7
  0,1,2,3,4,5,6,7,8,9,10,11,12
  0,0,0,1,2,2,3,4,5,6
  0,0,1,2,2,3,4,5,6,7,7
  0,0,1,2,3,3,4,5,6,7,8,9,10,11,12,12)
foreach(fanout 2 4)
  foreach(map IN LISTS osoe_tree_maps)
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    list(REMOVE_DUPLICATES ranks)
    list(LENGTH ranks nodes)
    add_test(NAME shmem.tree.k${fanout}.n${nodes}.p${count}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_FANOUT=${fanout}
              OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
              SHMEM_SYMMETRIC_SIZE=16M
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 150 --
              $<TARGET_FILE:shmem_test_multinode>)
    set_tests_properties(shmem.tree.k${fanout}.n${nodes}.p${count}
      PROPERTIES LABELS "tier2" TIMEOUT 180)
  endforeach()
endforeach()

# Work discovery on the send side (§3.10; slice 3 part C): 16 logical nodes,
# traffic on one connection, and a pass must drain and flush O(1) of them.
add_executable(shmem_test_work_discovery test-work-discovery.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_work_discovery PRIVATE
  ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_work_discovery PRIVATE osoe rt)
osoe_warnings(shmem_test_work_discovery)
add_test(NAME shmem.work-discovery
  COMMAND ${CMAKE_COMMAND} -E env SHMEM_SYMMETRIC_SIZE=16M
          $<TARGET_FILE:osoe-launch> -n 16 -m 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 -t 120
          -- $<TARGET_FILE:shmem_test_work_discovery>)
set_tests_properties(shmem.work-discovery PROPERTIES LABELS "tier2" TIMEOUT 150)

# The same program with §8.1's force-remote flag on, which is what makes
# osoe_transport_direct's call to the one locality function testable at all:
# without it a PE on another node already has no mapped heap here, so the
# answer is NULL whether the function is consulted or not, and the line that
# matters is the one that returns NULL for a PE which *is* on this node.
#
# The data path does not route yet, so this is the shmem_ptr half of the
# differential test rather than the whole of it. Multi-node maps only: with one
# logical node there is no peer to route to, and what force-remote should mean
# there is an open question (§8.1). All four multi-node poles, as its siblings
# run: SHARED must collapse to {self} on the uneven and the non-contiguous maps
# too.
foreach(map 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
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
# The same program under §8.1's force-remote flag (slice 3). It is registered
# now because the flag no longer touches the route: a put to a node-mate is
# still applied in the calling thread, and only the *pointer* is denied
# (job.h's two predicates). Until that split, this configuration aborted with
# "no connection toward node N" - 4 runs in 4 - which is what the test would
# have said had it been registered, and what three comments claimed could not
# happen. Multi-node maps only: the flag needs somewhere to be remote from, and
# is refused at init on one logical node - which is also why these do not run
# under ThreadSanitizer at all, where the multi-node maps of shmem.internode.*
# are already excluded for the reason given above (the ordering that makes a
# put safe runs through another process, and tsan reports a race it cannot
# disprove). With the single-node map unavailable to this flag, there is
# nothing left to run there.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
foreach(map 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.internode.force-remote.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1
            OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_internode>)
  set_tests_properties(shmem.internode.force-remote.${label} PROPERTIES LABELS "tier2"
    TIMEOUT 90)
endforeach()
endif()

# §8.2's heap-base shift (invariant 7): every PE maps its own heap at
# B + rank x OSOE_HEAP_SHIFT, so no two PEs of the job hold a symmetric object at
# the same address, and code that uses an address where it should use an
# offset - on the wire, or in the intranode path's translation - fails. Until
# this mode every PE mapped its own heap at its node's B, and every node chose
# the same B, so such code passed everything. A sample runs shifted in every
# preset; the whole of tier 2 runs shifted with OSOE_HEAP_SHIFT in ctest's
# environment. 2 MiB keeps every PE of these jobs within a few MiB of the
# candidates each sanitizer allows.
foreach(name rma amo amo-types wait wait-many signal team malloc barrier)
  add_test(NAME shmem.heap-shift.${name}.L4
    COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_SHIFT=2m
            $<TARGET_FILE:osoe-launch> -n 4 -t 60 -- $<TARGET_FILE:shmem_test_${name}>)
  set_tests_properties(shmem.heap-shift.${name}.L4 PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  foreach(map 0,0,1,1,2,2 0,1,2,0,1,0)
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    string(REPLACE "," "-" label "${map}")
    add_test(NAME shmem.heap-shift.internode.${label}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_SHIFT=2m
              OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
              $<TARGET_FILE:shmem_test_internode>)
    set_tests_properties(shmem.heap-shift.internode.${label} PROPERTIES LABELS "tier2"
      TIMEOUT 90)
  endforeach()
endif()
# OSOE_HEAP_SHIFT is a whole number of pages, and anything else is refused by name.
add_test(NAME shmem.heap-shift.invalid
  COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_SHIFT=100
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_rma>)
set_tests_properties(shmem.heap-shift.invalid PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_HEAP_SHIFT='100' is not a whole number of")

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
# PE 0 quiets toward it, PE 1 sees it blocked in osoe-stat with its sweep count
# rising, and dies, and PE 0 must end naming shmem_quiet. -k leaves the
# survivors to the library. Before slice 2 the quiet never swept: osoe-stat
# showed nothing for PE 0, and the job ran to the launcher's 15 s timeout.
add_executable(shmem_test_unswept-wait test-unswept-wait.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_unswept-wait PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_unswept-wait PRIVATE osoe rt)
osoe_warnings(shmem_test_unswept-wait)
add_test(NAME shmem.unswept-wait
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
          $<TARGET_FILE:osoe-launch> -n 3 -m 0,0,1 -t 15 -k --
          $<TARGET_FILE:shmem_test_unswept-wait>)
set_tests_properties(shmem.unswept-wait PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_quiet: the job is aborting on behalf of PE 1|waiting in shmem_quiet"
  FAIL_REGULAR_EXPRESSION "BLOCKED RECORD WRONG|QUIET RETURNED|QUIET DID NOT SWEEP")

# Diagnosability experiment 1 (§3.14, §5.3): one PE never reaches a barrier,
# and the node it is on is found by walking down the tree rather than by
# visiting every node. Eight logical nodes at k = 2, so the walk is three
# visits. PE 0 leaves without finalizing once it has the answer, which its node
# declares and the launcher finishes; the test is judged on what it printed.
add_executable(shmem_test_hang-walk test-hang-walk.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_hang-walk PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_hang-walk PRIVATE osoe rt)
osoe_warnings(shmem_test_hang-walk)
add_test(NAME shmem.hang-walk
  COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_FANOUT=2 SHMEM_SYMMETRIC_SIZE=16M
          $<TARGET_FILE:osoe-launch> -n 8 -m 0,1,2,3,4,5,6,7 -t 12 -k --
          $<TARGET_FILE:shmem_test_hang-walk>)
set_tests_properties(shmem.hang-walk PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "hang walk found node 6 in [0-9]+ visits"
  FAIL_REGULAR_EXPRESSION "CHECK[(]")

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
# The target's pool is C per connection plus (k + 1) x teams for the tree's
# forwards, node-wide (§3.8): with C = 1 and k = 2 that is 1 + 3 x 3 = 10, so
# eleven origin PEs each with a blocking get outstanding overflow it. Slice 3
# made the forward term node-wide; at the default k = 4 it is 16, and the
# seven origin PEs this test had until then stopped reaching it.
add_test(NAME shmem.reply-pool.overflow
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_POOL=overflow SHMEM_SYMMETRIC_SIZE=16m
          OSOE_COLLECTIVE_FANOUT=2
          $<TARGET_FILE:osoe-launch> -n 12 -m 0,0,0,0,0,0,0,0,0,0,0,1 -t 30
          -e 11:OSOE_AM_CREDITS=1 -- $<TARGET_FILE:shmem_test_reply-pool>)
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
# Keepalive asks whether a peer's kernel is there, and a stopped node's is
# (§3.15, wire.c): at its tightest - a probe after 1 s idle, then three 1 s
# apart - node 1 stopped for 6 s is still waited for, and the quiet completes.
# tier3.*.vanished-node is the other half: a node whose link is cut is found.
add_test(NAME shmem.stopped-node.keepalive
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STOP_MS=6000 OSOE_KEEPALIVE=1,1,3
          $<TARGET_FILE:osoe-launch> -n 3 -m 0,0,1 -t 30 --
          $<TARGET_FILE:shmem_test_stopped-node>)
set_tests_properties(shmem.stopped-node.keepalive PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "quiet completed after node 1 was stopped")

# The other half of D-6 (diagnosability review): a blocking get waits for one
# PE's reply, not a whole node's queue, so its no-progress fatal must name that
# PE and not only the node. Before this fix it named neither.
osoe_tier2_map_test(get-no-progress 0,1 OSOE_NO_PROGRESS_TIMEOUT=1)
set_tests_properties(shmem.get-no-progress.0-1 PROPERTIES
  PASS_REGULAR_EXPRESSION "shmem_get: no progress toward node 1 [(]PE 1[)] for [0-9]+ s"
  FAIL_REGULAR_EXPRESSION "GET RETURNED")

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
# And OSOE_SHM_CAPACITY_CHECK=0, for testing on a small /dev/shm: the same
# request is reported and the job runs.
add_test(NAME shmem.dev-shm-capacity.warned
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> warned)
set_tests_properties(shmem.dev-shm-capacity.heaps shmem.dev-shm-capacity.connections
  shmem.dev-shm-capacity.warned PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.dev-shm-capacity.warned PROPERTIES PASS_REGULAR_EXPRESSION
  "and 2 heaps.*continuing, since OSOE_SHM_CAPACITY_CHECK=0.*launcher exited 0")
set_tests_properties(shmem.dev-shm-capacity.heaps PROPERTIES PASS_REGULAR_EXPRESSION
  "/dev/shm has [0-9]+ MiB free, but this node's control segment and 2 heaps.*launcher exited 1")
set_tests_properties(shmem.dev-shm-capacity.connections PROPERTIES PASS_REGULAR_EXPRESSION
  "/dev/shm has [0-9]+ MiB free, but this node's connection area.*launcher exited 1"
  SKIP_RETURN_CODE 77)

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

# A job that dies, dies everywhere (§3.15; phase 1 step 4): PE 0 calls
# shmem_global_exit, or ends the job with a fatal, while every other PE computes
# outside the library, where only its agent can end it. Every rank must leave
# with the status - the caller's, or a fatal's failure - no rank may be killed
# by the launcher or outlast its computing, and for a global exit every rank's
# unflushed stdout must have been written (§9.1.6). See global-exit.sh. Across
# nodes a fatal travels as the lead's end of stream, which a peer takes for a
# death; a global exit's status travels as the abortive goodbye before it
# (abort_model.py), and a remote rank that left with the failure status instead
# fails these.
add_executable(shmem_test_global-exit test-global-exit.c)
target_link_libraries(shmem_test_global-exit PRIVATE osoe)
osoe_warnings(shmem_test_global-exit)
foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
  add_test(NAME shmem.global-exit.L${count}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> ${count} - 7 7)
  add_test(NAME shmem.global-exit.fatal.L${count}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> ${count} - fatal 1)
  set_tests_properties(shmem.global-exit.L${count} shmem.global-exit.fatal.L${count}
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endforeach()
foreach(map 0,1 0,0,1,1 0,1,2,0,1,0)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.global-exit.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> ${count} ${map} 7 7)
  add_test(NAME shmem.global-exit.fatal.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> ${count} ${map} fatal 1)
  set_tests_properties(shmem.global-exit.${label} shmem.global-exit.fatal.${label}
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endforeach()
# The same ended by a non-lead, which only its lead's agent can speak for: a
# global exit, a fatal - whose abortive goodbye then carries a fatal - and a
# global exit straight after shmem_init, which init must not return before the
# lead's agent runs to carry. Two ended while every other PE's main thread is
# inside exit, holding exit's lock: in start_pes's finalize, and in a handler
# of the program's before the library's, which a non-caller's _exit must not
# wait on. One with a handler on every PE, which only the caller's exit may
# run. And one ended mid-stream: the goodbye queued
# behind a stream, with one arriving, must survive the caller's end, which is
# a reset while input is unread, until the peer's kernel has acknowledged it.
foreach(case "nonlead;7;7;1;compute" "fatal.nonlead;fatal;1;1;compute"
             "immediate;7;7;1;immediate" "start-pes;7;7;0;start-pes"
             "returned;7;7;0;returned" "handlers;7;7;1;handlers")
  list(GET case 0 label)
  list(GET case 1 how)
  list(GET case 2 status)
  list(GET case 3 caller)
  list(GET case 4 mode)
  add_test(NAME shmem.global-exit.${label}.0-0-1-1
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 ${how} ${status} ${caller} ${mode})
  set_tests_properties(shmem.global-exit.${label}.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endforeach()
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_test(NAME shmem.global-exit.stream.0-0-1-1
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 0 stream)
  set_tests_properties(shmem.global-exit.stream.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  # The same with two wire buffers, so that a node's ring has often none free
  # when the goodbye arrives: the goodbye then waits in its kernel while the
  # node's own stream meets the caller's reset as a failed send, which must
  # stop the sends and nothing else (wire.h: send_error).
  add_test(NAME shmem.global-exit.stream.two-buffers.0-0-1-1
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 0 stream)
  set_tests_properties(shmem.global-exit.stream.two-buffers.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90 ENVIRONMENT "OSOE_WIRE_BUFFERS=2"
               PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endif()

# OSOE_KEEPALIVE is three counts or 0, and anything else is refused by name.
add_test(NAME shmem.keepalive.invalid
  COMMAND ${CMAKE_COMMAND} -E env OSOE_KEEPALIVE=5,0,3
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_global-exit> 7)
set_tests_properties(shmem.keepalive.invalid PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_KEEPALIVE='5,0,3' is not IDLE,INTERVAL,COUNT")

# A PE that dies without finalizing ends the job (§3.15, liveness.h): world
# rank DIES kills itself after a barrier while the others
# compute or wait, and every survivor must leave with the failure status. A
# sibling's death is the lead's pidfd to see and a lead's its siblings'; on two
# nodes the other node learns it from the abort. See dead-pe.sh.
add_executable(shmem_test_dead-pe test-dead-pe.c)
target_link_libraries(shmem_test_dead-pe PRIVATE osoe)
osoe_warnings(shmem_test_dead-pe)
foreach(case "sibling.L2;0,0;1" "lead.L2;0,0;0" "sibling.L4;0,0,0,0;3" "lead.L4;0,0,0,0;0"
             "sibling.0-0-1-1;0,0,1,1;1" "lead.0-0-1-1;0,0,1,1;0")
  list(GET case 0 label)
  list(GET case 1 map)
  list(GET case 2 dies)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  add_test(NAME shmem.dead-pe.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dead-pe.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_dead-pe> ${count} ${map} ${dies})
  set_tests_properties(shmem.dead-pe.${label}
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "DEAD PE OK")
endforeach()
# A lead that dies inside shmem_init, before any agent runs: its non-lead must
# find it from init's own sweeps. See dead-pe-init.sh.
add_test(NAME shmem.dead-pe.lead-in-init
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dead-pe-init.sh $<TARGET_FILE:osoe-launch>
          $<TARGET_FILE:shmem_test_dead-pe>)
set_tests_properties(shmem.dead-pe.lead-in-init
  PROPERTIES LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "DEAD LEAD IN INIT OK")

# A PE that dies while its siblings wait in the library ends the job, in every
# build, naming where each survivor was (§3.15): its lead's agent finds the
# death by its pidfd, and each survivor ends from its agent, which names the
# routine its thread waits in, or from that wait's sweep, which names it too.
# Every survivor must name its routine, on one node and on two, where a put
# first makes the barrier quiet inside its own wait. See death-in-wait.sh. The
# development watchdog's test until it was retired.
add_executable(shmem_test_death-in-wait test-death-in-wait.c)
target_link_libraries(shmem_test_death-in-wait PRIVATE osoe)
osoe_warnings(shmem_test_death-in-wait)
foreach(layout "L2;2;-" "L4;4;-" "0-0-1-1;4;0,0,1,1")
  foreach(case "barrier;shmem_barrier_all" "wait_until;shmem_uint64_wait_until")
    list(GET layout 0 label)
    list(GET layout 1 count)
    list(GET layout 2 map)
    list(GET case 0 where)
    list(GET case 1 routine)
    add_test(NAME shmem.death-in-wait.${where}.${label}
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/death-in-wait.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_death-in-wait> ${count} ${map} ${where} ${routine})
    set_tests_properties(shmem.death-in-wait.${where}.${label} PROPERTIES
      LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "DEATH IN WAIT OK")
  endforeach()
endforeach()

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
# The deprecated SMA_ spelling (spec §8, Annex F) must be honoured the same
# way, down to the same validation (conformance review F3): before F3's fix,
# bootstrap.c read SHMEM_SYMMETRIC_SIZE only and SMA_SYMMETRIC_SIZE=12q was
# silently ignored, so this job would have run instead of refusing.
add_test(NAME shmem.refuse.bad_size_sma
  COMMAND osoe-launch -n 2 -t 60 -e 0:SMA_SYMMETRIC_SIZE=12q -e 1:SMA_SYMMETRIC_SIZE=12q
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.bad_size_sma PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE='12q' is invalid")
# A zero heap is syntactically valid and never useful (conformance review F9 /
# omissions 11): every shmem_malloc would fail on every PE.
add_test(NAME shmem.refuse.zero_size
  COMMAND osoe-launch -n 1 -t 60 -e 0:SHMEM_SYMMETRIC_SIZE=0
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.zero_size PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE='0' is 0")
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
  shmem.refuse.bad_size_sma shmem.refuse.zero_size
  shmem.refuse.size_mismatch shmem.refuse.nonsymmetric_put PROPERTIES
  LABELS "tier2" TIMEOUT 90)

# A call before shmem_init, or after the last shmem_finalize, must be a named
# fatal rather than a segfault, a misleading message, a valid-looking wrong
# answer, or a silent success (conformance review F5). "before" needs no
# launcher - none of these routines reads the environment before the
# uninitialized check fires - so it runs directly; "after" needs a real job
# first, so it runs under osoe-launch.
add_executable(shmem_test_uninitialized test-uninitialized.c)
target_link_libraries(shmem_test_uninitialized PRIVATE osoe)
osoe_warnings(shmem_test_uninitialized)

foreach(case_expect
    "malloc|shmem_malloc: called before shmem_init"
    "free|shmem_free: called before shmem_init"
    "wait_until|shmem_long_wait_until: called before shmem_init"
    "barrier_all|shmem_barrier_all: called before shmem_init"
    "putmem|shmem_putmem: called before shmem_init"
    "team_my_pe|shmem_team_my_pe: called before shmem_init"
    "quiet|quiet: called before shmem_init"   # shmem_quiet wraps shmem_ctx_quiet (§7.3)
    "ptr|shmem_ptr: called before shmem_init")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 case)
  list(GET parts 1 expect)
  add_test(NAME shmem.refuse.uninitialized.before.${case}
    COMMAND shmem_test_uninitialized before ${case})
  set_tests_properties(shmem.refuse.uninitialized.before.${case} PROPERTIES
    LABELS "tier1" TIMEOUT 30 PASS_REGULAR_EXPRESSION "${expect}")
endforeach()

# The two the conformance review measured specifically after a real
# shmem_finalize: shmem_malloc used to segfault on the allocator's freed
# state, and shmem_ptr used to dereference a stale heap table instead of
# answering the NULL its own contract allows.
foreach(case_expect
    "malloc|shmem_malloc: called before shmem_init"
    "ptr|shmem_ptr: called before shmem_init")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 case)
  list(GET parts 1 expect)
  add_test(NAME shmem.refuse.uninitialized.after.${case}
    COMMAND osoe-launch -n 1 -t 30 -- $<TARGET_FILE:shmem_test_uninitialized> after ${case})
  set_tests_properties(shmem.refuse.uninitialized.after.${case} PROPERTIES
    LABELS "tier1" TIMEOUT 60 PASS_REGULAR_EXPRESSION "${expect}")
endforeach()

# Tier 3 (design §8.2, §8.3): tier 2's multi-node tests with each logical node
# in a network namespace of its own, over veth links netem shapes - a Pi-like
# link, the same with loss, and a small MTU (tools/launch/tier3.sh). They need
# root and change the machine's network state while they run, so they are
# registered only when asked for, in a build directory of their own - the tier3
# preset, `cmake --workflow --preset tier3` - and each runs the launcher through
# sudo -n, which returns every rank to the invoking user before the program.
#
# The subset is the tests whose traffic crosses a node boundary through the
# transport, each with the environment tier 2 gives it: the node-pair wire and
# the progress engine below the library; the internode maps, every operation
# and all three of §3.11's waits through two reply credits and a 2 KiB buffer;
# the AM barrier; a reply that full buffers must not starve; the tree at depth
# three and at thirteen uneven nodes; the rendezvous, against the namespaces'
# host list; the differential over §8.1's four poles; the floor, which
# measures the library on the shaped link; a global exit, whose status
# reaches the other nodes only as the abortive goodbye on the link (§3.15);
# and a node whose link is cut, which only TCP keepalive can find. Left out are
# the tests that never reach a socket, and those whose subject is a PE that
# dies or stops rather than the link.
option(OSOE_TIER3 "register the tier-3 tests, which need passwordless sudo" OFF)
if(OSOE_TIER3 AND CMAKE_C_FLAGS MATCHES "sanitize=thread")
  message(FATAL_ERROR "OSOE_TIER3: the multi-node data-path tests do not run under "
                      "ThreadSanitizer (shmem.internode's reason); use the tier3 preset")
endif()
if(OSOE_TIER3)
  add_executable(shmem_test_vanished-node test-vanished-node.c)
  target_link_libraries(shmem_test_vanished-node PRIVATE osoe)
  osoe_warnings(shmem_test_vanished-node)
  set(osoe_tier3 sudo -n ${PROJECT_SOURCE_DIR}/tools/launch/tier3.sh)
  set(osoe_tier3_wire OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_WIRE_BYTES=256
      OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919)
  # tier3.<shape>.<name>: PROGRAM [ARGS...] launched on MAP, with ENV's NAME=VALUE
  # pairs in the job's environment.
  function(osoe_tier3_test name shape map program)
    cmake_parse_arguments(PARSE_ARGV 4 tier3 "" "" "ENV;ARGS")
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    list(REMOVE_DUPLICATES ranks)
    list(LENGTH ranks nodes)
    add_test(NAME tier3.${shape}.${name}
      COMMAND ${osoe_tier3} ${shape} ${nodes} ${tier3_ENV}
              -- $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 300 --
              $<TARGET_FILE:${program}> ${tier3_ARGS})
    set_tests_properties(tier3.${shape}.${name} PROPERTIES LABELS "tier3" TIMEOUT 400)
  endfunction()
  foreach(shape pi lossy small)
    osoe_tier3_test(wire-pair ${shape} 0,1 shmem_test_wire_pair
                    ENV OSOE_WIRE_BYTES=64 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919)
    osoe_tier3_test(progress ${shape} 0,0,1,1 shmem_test_progress
                    ENV OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_WIRE_BYTES=256
                        OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919)
    foreach(map 0,1,2 0,0,1,1,2,2 0,0,0,1,2,2 0,1,2,0,1,0)
      string(REPLACE "," "-" label "${map}")
      osoe_tier3_test(internode.${label} ${shape} ${map} shmem_test_internode
                      ENV ${osoe_tier3_wire} OSOE_AM_CREDITS=2)
    endforeach()
    osoe_tier3_test(barrier-am.0-1-2 ${shape} 0,1,2 shmem_test_barrier_am
                    ENV ${osoe_tier3_wire})
    osoe_tier3_test(reply-starvation.0-1-1-1-1-1-1 ${shape} 0,1,1,1,1,1,1
                    shmem_test_reply-starvation
                    ENV OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_WIRE_BYTES=4096
                        OSOE_TEST_PUT_BYTES=984 SHMEM_SYMMETRIC_SIZE=16m)
    osoe_tier3_test(tree.k2.n8.p8 ${shape} 0,1,2,3,4,5,6,7 shmem_test_multinode
                    ENV OSOE_COLLECTIVE_FANOUT=2 ${osoe_tier3_wire} SHMEM_SYMMETRIC_SIZE=16M)
    osoe_tier3_test(tree.k4.n13.p16 ${shape} 0,0,1,2,3,3,4,5,6,7,8,9,10,11,12,12
                    shmem_test_multinode
                    ENV OSOE_COLLECTIVE_FANOUT=4 ${osoe_tier3_wire} SHMEM_SYMMETRIC_SIZE=16M)
    osoe_tier3_test(job.0-1-2-3 ${shape} 0,1,2,3 shmem_test_job)
    # A 64 MiB stream: at 1 Gbit/s the default gigabyte takes nine seconds a trial.
    osoe_tier3_test(floor ${shape} 0,1 shmem_test_floor ARGS 64)
    foreach(map 0,1 0,1,2,0,1,0)
      string(REPLACE "," ";" ranks "${map}")
      list(LENGTH ranks count)
      list(REMOVE_DUPLICATES ranks)
      list(LENGTH ranks nodes)
      string(REPLACE "," "-" label "${map}")
      string(REPLACE ";" " " wrap "${osoe_tier3};${shape};${nodes};--")
      add_test(NAME tier3.${shape}.global-exit.${label}
        COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
                sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_global-exit> ${count} ${map} 7 7)
      set_tests_properties(tier3.${shape}.global-exit.${label}
        PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    endforeach()
    # A global exit straight after init, and one mid-stream, where the link's
    # delay and loss are what the two orderings are for; the first again across
    # eight nodes, whose lead's mesh takes longest to connect.
    string(REPLACE ";" " " wrap "${osoe_tier3};${shape};8;--")
    add_test(NAME tier3.${shape}.global-exit.immediate.0-0-1-2-3-4-5-6-7
      COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
              sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 9 0,0,1,2,3,4,5,6,7 7 7 1 immediate)
    set_tests_properties(tier3.${shape}.global-exit.immediate.0-0-1-2-3-4-5-6-7
      PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    string(REPLACE ";" " " wrap "${osoe_tier3};${shape};2;--")
    foreach(case "immediate;1;immediate" "stream;0;stream")
      list(GET case 0 label)
      list(GET case 1 caller)
      list(GET case 2 mode)
      add_test(NAME tier3.${shape}.global-exit.${label}.0-0-1-1
        COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
                sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 ${caller} ${mode})
      set_tests_properties(tier3.${shape}.global-exit.${label}.0-0-1-1
        PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    endforeach()
    string(REPLACE ";" " " wrap "${osoe_tier3};${shape};2;OSOE_WIRE_BUFFERS=2;--")
    add_test(NAME tier3.${shape}.global-exit.stream.two-buffers.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
              sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 0 stream)
    set_tests_properties(tier3.${shape}.global-exit.stream.two-buffers.0-0-1-1
      PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    # A node that vanishes - its link cut once the job is ready - is found by TCP
    # keepalive at its tightest (§3.15, wire.c). See vanished-node.sh.
    foreach(map 0,1 0,0,1,1)
      string(REPLACE "," ";" ranks "${map}")
      list(LENGTH ranks count)
      string(REPLACE "," "-" label "${map}")
      string(REPLACE ";" " " wrap
             "${osoe_tier3};${shape};2;TIER3_CUT=1;OSOE_KEEPALIVE=1,1,3;--")
      add_test(NAME tier3.${shape}.vanished-node.${label}
        COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
                sh ${CMAKE_CURRENT_SOURCE_DIR}/vanished-node.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_vanished-node> ${count} ${map})
      set_tests_properties(tier3.${shape}.vanished-node.${label}
        PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "VANISHED NODE OK")
    endforeach()
    add_test(NAME tier3.${shape}.differential
      COMMAND ${CMAKE_COMMAND}
              "-DWRAP=${osoe_tier3};${shape}"
              -DLAUNCH=$<TARGET_FILE:osoe-launch>
              -DEXE=$<TARGET_FILE:shmem_test_differential>
              -DNRANKS=6 -DTIMEOUT=300
              "-DMAPS=0,0,0,0,0,0;0,1,2,3,4,5;0,0,0,1,2,2;0,1,2,0,1,0"
              "-DENVS=OSOE_AM_CHUNK=1024;OSOE_AM_BUFFER=2048;OSOE_AM_CREDITS=2;OSOE_WIRE_BYTES=256;OSOE_WIRE_SPLIT=2000;OSOE_WIRE_SEED=20260919;SHMEM_SYMMETRIC_SIZE=16m"
              -P ${PROJECT_SOURCE_DIR}/cmake/Differential.cmake)
    set_tests_properties(tier3.${shape}.differential PROPERTIES LABELS "tier3" TIMEOUT 1300)
  endforeach()
endif()
