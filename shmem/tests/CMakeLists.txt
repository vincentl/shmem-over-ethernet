# SPDX-License-Identifier: LGPL-3.0-or-later
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

  # The library's file and soname carry the project's version (cmake/OsoeVersion.cmake):
  # libosoe.so.<version>, and libosoe.so.<major> as its soname.
  add_test(NAME shmem.version.library
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:osoe> -DVERSION=${PROJECT_VERSION}
            -DSOVERSION=${PROJECT_VERSION_MAJOR}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckLibraryVersion.cmake)
  set_tests_properties(shmem.version.library PROPERTIES LABELS "tier1" TIMEOUT 30)
endif()

# The backend boundary (§4.10): no file above transport.h and internode.h may
# include, however indirectly, a header of the backend below. The two sides
# are defined as lists in the script, and adding `#include "progress.h"` to
# rma.c is the case it was written to fail (second review, roce-seam F6).
add_test(NAME shmem.boundary
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckBoundary.cmake)
set_tests_properties(shmem.boundary PROPERTIES LABELS "tier1" TIMEOUT 30)

# Every OSOE_ setting the library and its tools read is in the README, as
# `NAME` (the author, docs/decisions/2026-10-09.md, item 5): OSOE_TEST_ABORT_DELAY_MS
# had no row until its part's own documents gave it one, and no review had
# asked (CheckSettingsDocumented.cmake).
add_test(NAME shmem.settings-documented
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckSettingsDocumented.cmake)
set_tests_properties(shmem.settings-documented PROPERTIES LABELS "tier1" TIMEOUT 30)

# Every source file carries the license line, as the README says, but the
# specification's four examples: 82 evidence programs went without it until the
# review of v0.2 step 4's merge point (readiness Q5-1), since nothing checked
# it (CheckLicenseLines.cmake).
add_test(NAME shmem.license-lines
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckLicenseLines.cmake)
set_tests_properties(shmem.license-lines PROPERTIES LABELS "tier1" TIMEOUT 30)

# Design §3.16's rule: every atomic access in the library names acquire,
# release or seq_cst - no `relaxed`, no call without `_explicit`. Step D added
# 37 relaxed accesses that all four presets passed; the review of its merge
# point found them (CheckMemoryOrders.cmake).
add_test(NAME shmem.memory-orders
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckMemoryOrders.cmake)
set_tests_properties(shmem.memory-orders PROPERTIES LABELS "tier1" TIMEOUT 30)

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
          "-DC_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}"
          "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
          "-DURING_LIBRARIES=${LIBURING_LINK_LIBRARIES}"
          -DSINGLE_NODE=${OSOE_SINGLE_NODE}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckStaticLink.cmake)
set_tests_properties(shmem.static-link PROPERTIES LABELS "tier1" TIMEOUT 180)

# None of the four presets sets CMAKE_C_EXTENSIONS, so plain -std=c11 (design
# §1: "a C11 compiler") is otherwise never built. A nested configure and
# build, the same shape as shmem.static-link above. Portability review F12.
# 600 s: the whole library rebuilt, 49 s beside the release suite and past 180
# under eight busy loops on four cores (the round-three review's K3; Part 6).
add_test(NAME shmem.strict-c11
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
          -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/strict-c11-check
          -DC_COMPILER=${CMAKE_C_COMPILER}
          "-DC_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}"
          "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
          -DSINGLE_NODE=${OSOE_SINGLE_NODE}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckStrictC11.cmake)
set_tests_properties(shmem.strict-c11 PROPERTIES LABELS "tier1" TIMEOUT 600)

# The single-node build is configured from these same lists, and a line that
# holds in the full build alone - a property on a test only it registers -
# refuses it at configure time: shmem.helper-wake's RUN_SERIAL did, from the
# grant fields' merge (fcb9cf0) until this check, and nothing in this suite
# looked. So the full build configures the single-node build of its own kind
# too: configure and generate only, its suite being its own to run.
if(NOT OSOE_SINGLE_NODE)
  add_test(NAME shmem.single-node.configure
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/single-node-configure-check
            -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            "-DC_FLAGS=${CMAKE_C_FLAGS}"
            "-DEXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            "-DSHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
            -DSHARED_LIBS=${BUILD_SHARED_LIBS}
            -DTEST_PINS=${OSOE_TEST_PINS}
            -DC_COMPILER=${CMAKE_C_COMPILER}
            "-DC_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}"
            "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckSingleNodeConfigure.cmake)
  set_tests_properties(shmem.single-node.configure PROPERTIES LABELS "tier1" TIMEOUT 300)
endif()

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

# The chooser of a large collective's shape on its own (tier 1; v0.2 step 2):
# what the Pi's link and a loopback's choose on four nodes, and the C against
# tools/modelcheck/shape_timing.py on a grid of inputs the model wrote
# (shape-grid.txt; model.shape_timing.grid checks the model still writes it).
add_executable(shmem_test_collective_shape test-collective-shape.c ../src/collective-shape.c
  ../src/collective-tree.c ../src/collective-circulant.c)
target_include_directories(shmem_test_collective_shape PRIVATE ../src)
osoe_warnings(shmem_test_collective_shape)
add_test(NAME shmem.shape.choice COMMAND shmem_test_collective_shape choice)
add_test(NAME shmem.shape.grid
  COMMAND shmem_test_collective_shape grid ${CMAKE_CURRENT_SOURCE_DIR}/shape-grid.txt)
set_tests_properties(shmem.shape.choice shmem.shape.grid PROPERTIES LABELS "tier1" TIMEOUT 60)

# Träff's circulant rounds on their own (tier 1; v0.2 step 3): every element of
# every lead's result combined from every node exactly once, for every node
# count from 2 to 70, and the C's whole table against what
# tools/modelcheck/circulant_model.py digested (circulant-digest.txt;
# model.circulant.digest checks the model still writes it).
add_executable(shmem_test_circulant test-circulant.c ../src/collective-circulant.c)
target_include_directories(shmem_test_circulant PRIVATE ../src)
osoe_warnings(shmem_test_circulant)
add_test(NAME shmem.circulant.arithmetic
  COMMAND shmem_test_circulant ${CMAKE_CURRENT_SOURCE_DIR}/circulant-digest.txt)
set_tests_properties(shmem.circulant.arithmetic PROPERTIES LABELS "tier1" TIMEOUT 60)

# The §4.9 watched-range encoding on its own (tier 1): pure logic, and the
# property it must have is one a brute-force check can state directly.
add_executable(shmem_test_watch_range test-watch-range.c)
target_include_directories(shmem_test_watch_range PRIVATE ../src)
osoe_warnings(shmem_test_watch_range)
add_test(NAME shmem.watch-range COMMAND shmem_test_watch_range)
set_tests_properties(shmem.watch-range PROPERTIES LABELS "tier1" TIMEOUT 60)

# internode: these programs compile the internode backend's own files
if(OSOE_INTERNODE)
  # AM framing and the stream parser (tier 1): the same bytes in every split.
  add_executable(shmem_test_am test-am.c ../src/am.c)
  target_include_directories(shmem_test_am PRIVATE ../src)
  target_link_libraries(shmem_test_am PRIVATE osoe_base)
  osoe_warnings(shmem_test_am)
  add_test(NAME shmem.am COMMAND shmem_test_am)
  set_tests_properties(shmem.am PROPERTIES LABELS "tier1" TIMEOUT 120)
  osoe_internode_label(shmem.am)

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
    osoe_internode_label(shmem.am.malformed.${case})
  endforeach()

  # Deferred reply queues and the credit (tier 1): pure logic and one atomic.
  add_executable(shmem_test_reply test-reply.c ../src/reply.c)
  target_include_directories(shmem_test_reply PRIVATE ../src)
  target_link_libraries(shmem_test_reply PRIVATE osoe_base Threads::Threads)
  osoe_warnings(shmem_test_reply)
  add_test(NAME shmem.reply COMMAND shmem_test_reply)
  set_tests_properties(shmem.reply PROPERTIES LABELS "tier1" TIMEOUT 60)
  osoe_internode_label(shmem.reply)

  # The AM sizing policy (tier 1): pure arithmetic, compiled in.
  add_executable(shmem_test_am_size test-am-size.c ../src/connection-area.c ../src/am-configuration.c ../src/connection.c
    ../src/shm-object.c)
  target_include_directories(shmem_test_am_size PRIVATE ../src)
  target_link_libraries(shmem_test_am_size PRIVATE osoe_base rt)
  osoe_warnings(shmem_test_am_size)
  add_test(NAME shmem.am-size COMMAND shmem_test_am_size)
  set_tests_properties(shmem.am-size PROPERTIES LABELS "tier1" TIMEOUT 60)
  osoe_internode_label(shmem.am-size)

  # The map from peer to owner (owner-map.h), both ways, at every helper count a
  # node may run, and the connection area laid out by it: each writer's mark in
  # its owner's bitmap alone, and no two owners' words on one line (tier 1, one
  # process).
  add_executable(shmem_test_owner_map test-owner-map.c ../src/connection-area.c
    ../src/am-configuration.c ../src/connection.c ../src/shm-object.c)
  target_include_directories(shmem_test_owner_map PRIVATE ../src)
  target_link_libraries(shmem_test_owner_map PRIVATE osoe_base rt)
  osoe_warnings(shmem_test_owner_map)
  # Small buffers: the area of 300 nodes is opened too.
  add_test(NAME shmem.owner-map
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            $<TARGET_FILE:shmem_test_owner_map>)
  set_tests_properties(shmem.owner-map PROPERTIES LABELS "tier1" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "owner map: ok")
  # internode: compiles connection-area.c
  osoe_internode_label(shmem.owner-map)

  # The reservation protocol (tier 1): the real code under real threads, checking
  # the properties the models check. Writers are threads because that is what the
  # tsan preset can instrument; §8.2 says what that does and does not buy.
  add_executable(shmem_test_connection test-connection.c ../src/connection.c)
  target_include_directories(shmem_test_connection PRIVATE ../src)
  target_link_libraries(shmem_test_connection PRIVATE osoe_base Threads::Threads)
  osoe_warnings(shmem_test_connection)
  add_test(NAME shmem.connection COMMAND shmem_test_connection)
  set_tests_properties(shmem.connection PROPERTIES LABELS "tier1" TIMEOUT 120)
  osoe_internode_label(shmem.connection)

  # Filling the room (v0.2 step 4's first rung): the reservation that takes as
  # much of the open buffer as there is, a writer's messages sent in parts, under
  # the same harness and the model's `fill` properties (reservation_model.py).
  add_executable(shmem_test_connection_fill test-connection-fill.c ../src/connection.c)
  target_include_directories(shmem_test_connection_fill PRIVATE ../src)
  target_link_libraries(shmem_test_connection_fill PRIVATE osoe_base Threads::Threads)
  osoe_warnings(shmem_test_connection_fill)
  add_test(NAME shmem.connection.fill COMMAND shmem_test_connection_fill)
  set_tests_properties(shmem.connection.fill PROPERTIES LABELS "tier1" TIMEOUT 120)
  osoe_internode_label(shmem.connection.fill)
endif()

# The compressed rank map on its own (tier 1): progressions, runs, lookups against
# the uncompressed map, the refusals, and the sizes at 4,096 x 256 (§6.2).
add_executable(shmem_test_rank-map test-rank-map.c ../src/rank-map.c)
target_include_directories(shmem_test_rank-map PRIVATE ../src)
osoe_warnings(shmem_test_rank-map)
add_test(NAME shmem.rank-map COMMAND shmem_test_rank-map)
set_tests_properties(shmem.rank-map PROPERTIES LABELS "tier1" TIMEOUT 60)

# internode: these compile wire.c, ring.c and listener.c, and link liburing
if(OSOE_INTERNODE)
  # The node-pair wire on its own (tier 1): both ends in one process, so no
  # launcher. wire.c is compiled in, since libosoe hides it, and the test supplies
  # its own osoe_fatal so a failure inside it fails the test.
  add_executable(shmem_test_wire test-wire.c ../src/wire.c ../src/wire-settings.c ../src/ring.c ../src/listener.c
    ../src/shm-object.c)
  target_include_directories(shmem_test_wire PRIVATE ../src)
  target_link_libraries(shmem_test_wire PRIVATE osoe_base rt PkgConfig::LIBURING)
  osoe_warnings(shmem_test_wire)
  add_test(NAME shmem.wire COMMAND shmem_test_wire)
  set_tests_properties(shmem.wire PROPERTIES LABELS "tier1" TIMEOUT 120)
  osoe_internode_label(shmem.wire)
  # The zero-copy send (wire.h): the same streams through two regions lent to
  # the kernel by reference, each refilled only once it owes nothing for it.
  add_test(NAME shmem.wire.zero-copy COMMAND shmem_test_wire zero-copy)
  set_tests_properties(shmem.wire.zero-copy PROPERTIES LABELS "tier1" TIMEOUT 120)
  osoe_internode_label(shmem.wire.zero-copy)
  # And with every send by reference refused for want of locked memory, the
  # ring made first (locked-memory.h; ring.c): each sending wire refused once,
  # lending nothing after, and every byte arriving by copy. Skipped where the
  # process may lock memory whatever its limit (CAP_IPC_LOCK).
  add_test(NAME shmem.wire.zero-copy-refused COMMAND shmem_test_wire zero-copy-refused)
  set_tests_properties(shmem.wire.zero-copy-refused PROPERTIES LABELS "tier1" TIMEOUT 120
    SKIP_REGULAR_EXPRESSION "SKIPPED: this process may lock memory")
  osoe_internode_label(shmem.wire.zero-copy-refused)
  # The peek's misuses (wire.h), each asserted by its message as the parser's
  # malformed segments are: a consume past the peek, a second consume of it, and
  # in the ASan preset alone, which poisons a provided buffer while it is the
  # kernel's (ring.c), a read of one after its consume.
  set(wire_misuses
    "consume-past-peek|consumed where the last peek showed"
    "consume-twice|consumed where the last peek showed 0")
  if(CMAKE_C_FLAGS MATCHES "sanitize=[a-z,]*address")
    list(APPEND wire_misuses "read-after-consume|use-after-poison")
  endif()
  foreach(case_expect IN LISTS wire_misuses)
    string(REPLACE "|" ";" parts "${case_expect}")
    list(GET parts 0 case)
    list(GET parts 1 expect)
    add_test(NAME shmem.wire.misuse.${case} COMMAND shmem_test_wire ${case})
    set_tests_properties(shmem.wire.misuse.${case} PROPERTIES
      LABELS "tier1" TIMEOUT 60 PASS_REGULAR_EXPRESSION "${expect}")
    osoe_internode_label(shmem.wire.misuse.${case})
  endforeach()

  # The wire between two processes (tier 2). Two logical nodes, one PE each, so
  # each process is a node lead - the only role that touches a wire (§3.12). It
  # does not call shmem_init: it tests the wire on its own, below the library,
  # and takes its identity straight from the launcher environment.
  # The seed is fixed: §3.14 wants an injected fault reproducible, and without it
  # the pattern would follow the launcher's pid and differ every run.
  add_executable(shmem_test_wire_pair test-wire-pair.c ../src/wire.c ../src/wire-settings.c ../src/ring.c
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
  osoe_internode_label(shmem.wire-pair)
endif()

# Multi-process tests (tier 2): each program runs under osoe-launch at every
# listed local PE count, as shmem.<name>.L<count>.
set(OSOE_LAUNCH_COUNTS 1 2 4)

if(OSOE_INTERNODE)
  # The internode phase of a barrier (tier 2). Three nodes at least: with two,
  # the root's own arrival and the single message it receives are the first and
  # the last, so releasing on any arrival is indistinguishable from releasing on
  # all of them - and that mutation passed a two-node test.
  add_executable(shmem_test_barrier_am test-barrier-am.c
    ../src/progress.c ../src/collective-tree.c ../src/am.c ../src/connection.c ../src/connection-area.c ../src/am-configuration.c
    ../src/reply.c ../src/wire.c ../src/wire-settings.c ../src/ring.c ../src/listener.c ../src/bootstrap.c
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
    osoe_internode_label(shmem.barrier-am.${label})
  endforeach()
endif()

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

# The "blocked in" record read while it is rewritten (§3.14; blocked.c): the
# library's writer on one CPU, both readers on another - its own, which the
# deadlock detector uses, and osoe-stat's, through a segment whose table points
# into the same slot. The round-three review's litmus (K1) on the code itself;
# it can fail only where loads are reordered, aarch64 among them. About 4 s.
add_executable(shmem_test_blocked-record test-blocked-record.c test-blocked-record-stat.c
  ../src/blocked.c ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_blocked-record PRIVATE ../src
  ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_blocked-record PRIVATE osoe_base rt Threads::Threads)
osoe_warnings(shmem_test_blocked-record)
add_test(NAME shmem.blocked-record COMMAND shmem_test_blocked-record)
set_tests_properties(shmem.blocked-record PROPERTIES LABELS "tier1" TIMEOUT 60)

# Where a node listens when its own name resolves to a loopback address, as
# Debian's /etc/hosts maps a host's own name to 127.0.1.1 (§6.2, listener.h).
add_executable(shmem_test_own-address test-own-address.c)
target_include_directories(shmem_test_own-address PRIVATE ../src)
osoe_warnings(shmem_test_own-address)
add_test(NAME shmem.own-address COMMAND shmem_test_own-address)
set_tests_properties(shmem.own-address PROPERTIES LABELS "tier1" TIMEOUT 30
  PASS_REGULAR_EXPRESSION "OWN ADDRESS OK")

# The bootstrap against launcher environments (§6.1, §8.2): each fixture in
# slurm-fixtures/ is one task's environment - captured from a real srun, or
# written from Slurm's documentation and marked so - replayed with nothing of
# the test's own, and read as the task it was or refused by name. See
# test-bootstrap.c and slurm-fixtures/README.md.
add_executable(shmem_test_bootstrap test-bootstrap.c ../src/bootstrap.c)
target_include_directories(shmem_test_bootstrap PRIVATE ../src)
target_link_libraries(shmem_test_bootstrap PRIVATE osoe_base)
osoe_warnings(shmem_test_bootstrap)
file(GLOB osoe_slurm_fixtures CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/slurm-fixtures/*.env)
foreach(fixture IN LISTS osoe_slurm_fixtures)
  get_filename_component(label ${fixture} NAME_WE)
  add_test(NAME shmem.bootstrap.${label} COMMAND shmem_test_bootstrap ${fixture})
  set_tests_properties(shmem.bootstrap.${label} PROPERTIES LABELS "tier1" TIMEOUT 30
    PASS_REGULAR_EXPRESSION "FIXTURE OK")
endforeach()

# internode: shmem_test_job compiles am.c, rendezvous.c and listener.c, on one-node maps too
if(OSOE_INTERNODE)
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
    osoe_internode_label(shmem.job.${label})
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
  osoe_internode_label(shmem.job.duplicate-rank shmem.job.unclaimed-rank)

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
    osoe_internode_label(shmem.job.duplicate-rank.slow-message)
  endif()
endif()

# A refusal must end the job, or say why it cannot: the round-three review's R8
# found a PE that outlived the refusal of another waiting for ever and printing
# nothing, which a suite that lets the launcher end the survivors cannot see. These
# tests run the job under osoe-launch -k (refusal.sh), which leaves them alone, so
# that a hang is the launcher's timeout. Of RANKS ranks, WAITING are left to it:
# those that wait for a node or a PE that will not come, whose wait's notice must
# name what is missing before then - after 30 s in a job, after
# OSOE_TEST_NOTICE_SECONDS here - since no wait gives up because time has passed
# (§3.15). Every other rank ends by itself with a failure status, and each PATTERN
# is written before the timeout. WAITING 0 is a refusal that ends the whole job.
#
#   osoe_refusal_test(NAME RANKS WAITING [SECONDS S] [ENV NAME=VALUE...]
#                     [LAUNCH LAUNCHER-ARGS...] [PATTERNS PATTERN...]
#                     PROGRAM WORDS...)
function(osoe_refusal_test name ranks waiting)
  cmake_parse_arguments(PARSE_ARGV 3 refusal "" "SECONDS" "ENV;LAUNCH;PATTERNS;PROGRAM")
  if(NOT refusal_SECONDS)
    set(refusal_SECONDS 8)
  endif()
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_NOTICE_SECONDS=2 ${refusal_ENV}
            sh ${CMAKE_CURRENT_SOURCE_DIR}/refusal.sh $<TARGET_FILE:osoe-launch>
            ${ranks} ${waiting} ${refusal_PATTERNS}
            -- ${refusal_LAUNCH} -t ${refusal_SECONDS} -- ${refusal_PROGRAM})
  math(EXPR refusal_budget "${refusal_SECONDS} + 30")
  set_tests_properties(${name} PROPERTIES LABELS "tier2" TIMEOUT ${refusal_budget}
    PASS_REGULAR_EXPRESSION "REFUSAL OK")
endfunction()

# What the notice of a lead that dials its parent says went wrong, which is
# "Connection refused" when the parent has ended and nothing listens on its
# candidate ports - unless something does: another job's lead of this machine,
# which tests running beside each other make likely enough to see (one run in a
# hundred), and whose answer is "not this node", or a listener that does not
# answer. Nothing else will do: the error must be the dial's, not "not tried yet".
set(osoe_dial_error
    "(Connection refused|no answer to the hello on a candidate port|its candidate ports answered for another node or job)")

if(OSOE_INTERNODE)
  # The values every node must agree on are checked up the rendezvous tree, each
  # by the parent that sees the disagreement, naming both nodes (§6.2). Node 2's
  # parent is node 0 at the tree's fan-out of 8. The root that refuses ends, and
  # its callers see their connection close: each of them ends, naming the lead it
  # lost.
  osoe_refusal_test(shmem.job.disagree.symmetric-size 3 0
    LAUNCH -n 3 -m 0,1,2 -e 2:SHMEM_SYMMETRIC_SIZE=64M
    PATTERNS "node 2 [(]or a node below it[)] has SHMEM_SYMMETRIC_SIZE [0-9]+; node 0 has"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
             "node 2: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_refusal_test(shmem.job.disagree.fanout 3 0
    LAUNCH -n 3 -m 0,1,2 -e 2:OSOE_COLLECTIVE_FANOUT=2
    PATTERNS "node 2 [(]or a node below it[)] has OSOE_COLLECTIVE_FANOUT=2; node 0 has 4"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
             "node 2: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)

  # OSOE_MAX_TEAMS is agreed the same way (§5.7): a split reduces every parent
  # PE's mask of numbers, which must be one length, and it lays out each PE's
  # internal region, whose offsets every node must compute alike. Two nodes, the
  # second's lead given 128 numbers to the first's default 256.
  osoe_refusal_test(shmem.job.disagree.max-teams 2 0
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_MAX_TEAMS=128
    PATTERNS "node 1 [(]or a node below it[)] has OSOE_MAX_TEAMS=128; node 0 has 256"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.disagree.symmetric-size shmem.job.disagree.fanout
    shmem.job.disagree.max-teams)
endif()

# Between the PEs of one node too: the lead sizes the team blocks, the rows of
# published epochs and the internal regions by its count, and a PE with
# another would index them by its own (node.c). A real program, since it is the
# node's rendezvous and not the job's that sees it. The PE that finds the
# difference names it and the lead, which has only the node's segment to hear it
# through, ends with "another local PE failed".
osoe_refusal_test(shmem.max-teams.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_MAX_TEAMS=128
  PATTERNS "osoe: PE 1: OSOE_MAX_TEAMS differs between PEs: the local lead has 256, this PE has 128"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
# And the lead's count the larger, whose segment then has room for more than
# the member counts on: refused the same way.
osoe_refusal_test(shmem.max-teams.local-disagree.lead 2 0
  LAUNCH -n 2 -e 0:OSOE_MAX_TEAMS=128
  PATTERNS "osoe: PE 1: OSOE_MAX_TEAMS differs between PEs: the local lead has 128, this PE has 256"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)

# And outside its range, or not a number, it is refused by name at init: at
# least one team beyond the three predefined, and at most 1,024 - by
# shmem_init in either build (bootstrap.c).
foreach(value 3 1025 x)
  add_test(NAME shmem.max-teams.invalid.${value}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_MAX_TEAMS=${value}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.max-teams.invalid.${value} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "OSOE_MAX_TEAMS='${value}' is not between 4 and 1024")
endforeach()
if(OSOE_INTERNODE)
  # OSOE_COLLECTIVE_PIPELINE too (§5.8): the pieces each lead keeps in flight
  # place them in the internal region, so a node with another number puts a piece
  # where its parent keeps another. Two nodes, the second's lead given 4 to the
  # first's default 2; and outside 1 to 8, or not a number, refused at init.
  osoe_refusal_test(shmem.job.disagree.pipeline 2 0
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_COLLECTIVE_PIPELINE=2
    PATTERNS "node 1 [(]or a node below it[)] has OSOE_COLLECTIVE_PIPELINE=2; node 0 has 4"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.disagree.pipeline)
endif()
foreach(value 0 9 x)
  add_test(NAME shmem.collective-pipeline.invalid.${value}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_PIPELINE=${value}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.collective-pipeline.invalid.${value} PROPERTIES LABELS "tier2"
    TIMEOUT 60 PASS_REGULAR_EXPRESSION "OSOE_COLLECTIVE_PIPELINE='${value}' is not between 1 and 8")
endforeach()

# Both are agreed between the PEs of one node as well, as OSOE_MAX_TEAMS is:
# they size every PE's internal region, and a created team's local lead may be
# any PE of the node, building its tree and keeping its window by its own
# (node.c, open_control).
osoe_refusal_test(shmem.collective-fanout.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_COLLECTIVE_FANOUT=2
  PATTERNS "osoe: PE 1: OSOE_COLLECTIVE_FANOUT differs between PEs: the local lead has 8, this PE has 2"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
osoe_refusal_test(shmem.collective-pipeline.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_COLLECTIVE_PIPELINE=2
  PATTERNS "osoe: PE 1: OSOE_COLLECTIVE_PIPELINE differs between PEs: the local lead has 4, this PE has 2"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)

# OSOE_SMALL_COLLECTIVE_BYTES the same three ways (the team redesign's step E):
# it sizes every team number's small rows, and every lead must decide alike
# which data collectives are small - a lead that took one as large would wait
# for a grant its parent never sends. Between nodes, the second's lead given
# 2,048 to the first's default 1,024; outside 128 to 16,384, or not a number,
# refused at init; and between the PEs of one node.
if(OSOE_INTERNODE)
  osoe_refusal_test(shmem.job.disagree.small-collective-bytes 2 0
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_SMALL_COLLECTIVE_BYTES=2048
    PATTERNS "node 1 [(]or a node below it[)] has OSOE_SMALL_COLLECTIVE_BYTES=2048; node 0 has 1024"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.disagree.small-collective-bytes)
endif()
foreach(value 127 16385 x)
  add_test(NAME shmem.small-collective-bytes.invalid.${value}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_SMALL_COLLECTIVE_BYTES=${value}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.small-collective-bytes.invalid.${value} PROPERTIES LABELS "tier2"
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "OSOE_SMALL_COLLECTIVE_BYTES='${value}' is not between 128 and 16384")
endforeach()
osoe_refusal_test(shmem.small-collective-bytes.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_SMALL_COLLECTIVE_BYTES=128
  PATTERNS "osoe: PE 1: OSOE_SMALL_COLLECTIVE_BYTES differs between PEs: the local lead has 1024, this PE has 128"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)

# The collective shape's setting and the link set by hand (v0.2 step 2): every
# node must choose a collective's shape from the same numbers, or two would
# build different trees for one call. Refused by name outside their values, the
# link's three alone or not at all; held alike between the PEs of a node and,
# below, between nodes.
# Each refusal: a name, the variables set (separated by ^), the pattern.
set(osoe_link_refusals
  "shape-bogus|OSOE_COLLECTIVE_SHAPE=bogus|OSOE_COLLECTIVE_SHAPE='bogus' is not link, tree or fixed"
  "hop-0|OSOE_LINK_HOP_NS=0^OSOE_LINK_MESSAGE_NS=7000^OSOE_LINK_BYTES_PER_SECOND=116000000|OSOE_LINK_HOP_NS='0' is not between 1 and 10000000000"
  "bandwidth-x|OSOE_LINK_HOP_NS=86000^OSOE_LINK_MESSAGE_NS=7000^OSOE_LINK_BYTES_PER_SECOND=x|OSOE_LINK_BYTES_PER_SECOND='x' is not between 1 and 10000000000000"
  "message-too-large|OSOE_LINK_HOP_NS=86000^OSOE_LINK_MESSAGE_NS=1000000001^OSOE_LINK_BYTES_PER_SECOND=116000000|OSOE_LINK_MESSAGE_NS='1000000001' is not between 0 and 1000000000"
  "hop-alone|OSOE_LINK_HOP_NS=86000|set together or not at all: OSOE_LINK_HOP_NS is set and OSOE_LINK_MESSAGE_NS is not"
  "bandwidth-alone|OSOE_LINK_BYTES_PER_SECOND=116000000|set together or not at all: OSOE_LINK_BYTES_PER_SECOND is set and OSOE_LINK_HOP_NS is not")
foreach(refusal IN LISTS osoe_link_refusals)
  string(REPLACE "|" ";" parts "${refusal}")
  list(GET parts 0 what)
  list(GET parts 1 variables)
  list(GET parts 2 pattern)
  string(REPLACE "^" ";" env "${variables}")
  add_test(NAME shmem.collective-shape.invalid.${what}
    COMMAND ${CMAKE_COMMAND} -E env ${env}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.collective-shape.invalid.${what} PROPERTIES LABELS "tier2"
    TIMEOUT 60 PASS_REGULAR_EXPRESSION "${pattern}")
endforeach()
osoe_refusal_test(shmem.collective-shape.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_COLLECTIVE_SHAPE=fixed
  PATTERNS "osoe: PE 1: OSOE_COLLECTIVE_SHAPE differs between PEs: the local lead has link, this PE has fixed"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
osoe_refusal_test(shmem.link.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_LINK_HOP_NS=86000 -e 1:OSOE_LINK_MESSAGE_NS=7000
         -e 1:OSOE_LINK_BYTES_PER_SECOND=116000000
  PATTERNS "osoe: PE 1: OSOE_LINK_[*] differ between PEs: the local lead has a hop of 0 ns, a message cost of 0 ns and 0 bytes a second, this PE 86000, 7000 and 116000000"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
if(OSOE_INTERNODE)
  osoe_refusal_test(shmem.job.disagree.collective-shape 2 0
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_COLLECTIVE_SHAPE=fixed
    PATTERNS "node 1 [(]or a node below it[)] has OSOE_COLLECTIVE_SHAPE=fixed; node 0 has link"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_refusal_test(shmem.job.disagree.link 2 0
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_LINK_HOP_NS=86000 -e 1:OSOE_LINK_MESSAGE_NS=7000
           -e 1:OSOE_LINK_BYTES_PER_SECOND=116000000
    PATTERNS "node 1 [(]or a node below it[)] has OSOE_LINK_[*] of a hop of 86000 ns, a message cost of 7000 ns and 116000000 bytes a second; node 0 has 0, 0 and 0"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.disagree.collective-shape shmem.job.disagree.link)
endif()

# The link the shapes are chosen from, as every PE's start-up line shows it
# under SHMEM_DEBUG (link.sh; link-measure.h): measured by the root's lead and
# the same on every PE, on two nodes and on three, where the root has two
# children, and with node 1 holding the fewest and smallest lanes the settings
# allow; as set by OSOE_LINK_*; and none at all where the shape is fixed or
# the job is one node.
foreach(case "measured;0,0,1,1" "measured;0,1,2" "few-lanes;0,1" "set;0,0,1,1" "fixed;0,1"
             "none;0,0")
  list(GET case 0 mode)
  list(GET case 1 map)
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.link.${mode}.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/link.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_barrier> ${map} ${mode})
  set_tests_properties(shmem.link.${mode}.${label} PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "LINK OK")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.link.${mode}.${label})
  endif()
endforeach()

# The limit on each connection's unsent bytes (v0.2 step 4; wire-settings.h), as
# each lead's agent says it under SHMEM_DEBUG once the link is known, with the
# connections it reached (unsent.sh): from the link measured, set by hand above
# and below the floor, fixed, the setting's own and 0, and nothing on one node.
foreach(case "measured;0,0,1,1" "set;0,1" "floor;0,1,2" "fixed;0,1" "by-hand;0,1" "none;0,1"
             "one-node;0,0")
  list(GET case 0 mode)
  list(GET case 1 map)
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  string(REPLACE "," "-" label "${map}")
  add_test(NAME shmem.unsent.${mode}.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/unsent.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_barrier> ${map} ${mode})
  set_tests_properties(shmem.unsent.${mode}.${label} PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "UNSENT OK")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.unsent.${mode}.${label})
  endif()
endforeach()

if(OSOE_INTERNODE)
  # The node count is the value that shapes the waits: a lead expects children by
  # its own count, so one given too many waits for children that do not exist.
  # The hello carries the count and a lead calls its parent before it waits for
  # its children, so the first edge that disagrees refuses the job, rather than
  # every lead waiting with no message (tools/modelcheck/rendezvous_model.py).
  # Node 1 given 10 nodes expects a child 9; node 0 given 9 expects 1 to 8.
  #
  # Without a secret nothing a caller says can be proved, so a lead judges a hello
  # as it arrives and ends at the first whose counts differ, before it has answered
  # it. Its caller is left with a closed connection and then with a port nobody
  # holds, and retries; its notice says what it sees - the node it calls, the host
  # list's entry for it, the address dialled, "Connection refused" (the round-three
  # review's N1: nothing was said at all). With a secret the lead answers first, and
  # the callers it answered end: see below. Node 2's hello is a good one, and node 2
  # ends if the lead answered it before it read node 1's, or else is one more node
  # retrying against a closed port. Which comes first is not the test's to choose,
  # so it requires either of node 2, and node 1 to wait.
  osoe_refusal_test(shmem.job.disagree.node-count 3 1-2
    LAUNCH -n 3 -m 0,1,2 -e 1:OSOE_STEP_NUM_NODES=10 -e "1:OSOE_STEP_NODELIST=127.0.0.[1-10]"
    PATTERNS "node 0, launched for 3 tasks on 3 nodes, was called by node 1, launched for 3 tasks on 10 nodes"
             "osoe_notice: shmem_init on node 1 has waited [0-9]+ s for node 0 [(]127[.]0[.]0[.]1, at 127[.]0[.]0[.]1[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: ${osoe_dial_error}"
             "node 2: the rendezvous lost node 0, its parent in the rendezvous tree|osoe_notice: shmem_init on node 2 has waited [0-9]+ s for node 0 [(]127[.]0[.]0[.]1, at 127[.]0[.]0[.]1[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: ${osoe_dial_error}"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  # The same with one caller: node 1 is a leaf in either view, and has nothing to
  # wait for but the lead it called, which ended on its hello and left it
  # the refused connection to retry.
  osoe_refusal_test(shmem.job.disagree.leaf-node-count 2 1
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_STEP_NUM_NODES=3 -e "1:OSOE_STEP_NODELIST=127.0.0.[1-3]"
    PATTERNS "node 0, launched for 2 tasks on 2 nodes, was called by node 1, launched for 2 tasks on 3 nodes"
             "osoe_notice: shmem_init on node 1 has waited [0-9]+ s for node 0 [(]127[.]0[.]0[.]1, at 127[.]0[.]0[.]1[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: ${osoe_dial_error}"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_refusal_test(shmem.job.disagree.root-node-count 3 2
    LAUNCH -n 3 -m 0,1,2 -e 0:OSOE_STEP_NUM_NODES=9 -e "0:OSOE_STEP_NODELIST=127.0.0.[1-9]"
    PATTERNS "node 0, launched for 3 tasks on 9 nodes, was called by node [12], launched for 3 tasks on 3 nodes"
             "osoe_notice: shmem_init on node 1 has waited [0-9]+ s for node 0 [(]127[.]0[.]0[.]1, at 127[.]0[.]0[.]1[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: ${osoe_dial_error}"
             "osoe_notice: shmem_init on node 2 has waited [0-9]+ s for node 0 [(]127[.]0[.]0[.]1, at 127[.]0[.]0[.]1[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: ${osoe_dial_error}"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.disagree.node-count shmem.job.disagree.leaf-node-count
    shmem.job.disagree.root-node-count)

  # OSOE_JOB_SECRET (§6.2, §10): leads that share it meet; a lead that has none,
  # or a different one, is refused by name - by the proof in the hello, which a
  # lead that does not know the secret cannot make.
  set(osoe_secret 0123456789abcdef0123456789abcdef)
  add_test(NAME shmem.job.secret
    COMMAND ${CMAKE_COMMAND} -E env OSOE_JOB_SECRET=${osoe_secret}
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 30 -- $<TARGET_FILE:shmem_test_job>)
  set_tests_properties(shmem.job.secret PROPERTIES LABELS "tier2" TIMEOUT 60)
  osoe_internode_label(shmem.job.secret)
  # A caller that finds the lead's answer is not one it can accept - a secret on
  # one side only, or two - ends by itself naming it, before it confirms. The lead
  # it called has not counted it and waits for it still: its notice names the node
  # and the port it waits on.
  osoe_refusal_test(shmem.job.secret.missing 2 1
    ENV OSOE_JOB_SECRET=${osoe_secret}
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_JOB_SECRET=
    PATTERNS "node 0 has an OSOE_JOB_SECRET and node 1 has none"
             "osoe_notice: shmem_init on node 0 has waited [0-9]+ s for node 1, its child in the rendezvous tree, to call it on port [0-9]+"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_refusal_test(shmem.job.secret.different 2 1
    ENV OSOE_JOB_SECRET=${osoe_secret}
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_JOB_SECRET=fedcba9876543210fedcba9876543210
    PATTERNS "a proof of a different OSOE_JOB_SECRET"
             "osoe_notice: shmem_init on node 0 has waited [0-9]+ s for node 1, its child in the rendezvous tree, to call it on port [0-9]+"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.secret.missing shmem.job.secret.different)

  # A genuine mislaunch with a secret. The lead judges a hello's counts and claims
  # only once its caller has proved the secret (listener.h), so that a host that
  # does not know it cannot end a node's start-up with one hello (the round-three
  # review's N2); it answers, then, and refuses its caller by name afterwards. A
  # caller that sees its connection close ends, naming the lead it lost, where
  # without a secret it retried a refused connection for ever
  # (shmem.job.disagree.leaf-node-count). Two nodes, so that one caller is in
  # question and the order of two is not: node 0 given 3 nodes, and in the next
  # test node 1, a leaf in either view, given 3. In both the lead names both counts
  # and node 1 ends.
  osoe_refusal_test(shmem.job.secret.disagree.root-node-count 2 0
    ENV OSOE_JOB_SECRET=${osoe_secret}
    LAUNCH -n 2 -m 0,1 -e 0:OSOE_STEP_NUM_NODES=3 -e "0:OSOE_STEP_NODELIST=127.0.0.[1-3]"
    PATTERNS "node 0, launched for 2 tasks on 3 nodes, was called by node 1, launched for 2 tasks on 2 nodes"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_refusal_test(shmem.job.secret.disagree.leaf-node-count 2 0
    ENV OSOE_JOB_SECRET=${osoe_secret}
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_STEP_NUM_NODES=3 -e "1:OSOE_STEP_NODELIST=127.0.0.[1-3]"
    PATTERNS "node 0, launched for 2 tasks on 2 nodes, was called by node 1, launched for 2 tasks on 3 nodes"
             "node 1: the rendezvous lost node 0, its parent in the rendezvous tree"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  # Node 1 given 10 nodes is a lead of children in its own view, and waits for the
  # first of them, node 9, before it reads anything from the lead it called. The
  # lead ends naming both counts, and node 1 is left waiting for a node that its
  # real job does not have, which its notice names: as ever, a lead given too large
  # a count waits for children that do not exist (rendezvous.c).
  osoe_refusal_test(shmem.job.secret.disagree.node-count 2 1
    ENV OSOE_JOB_SECRET=${osoe_secret}
    LAUNCH -n 2 -m 0,1 -e 1:OSOE_STEP_NUM_NODES=10 -e "1:OSOE_STEP_NODELIST=127.0.0.[1-10]"
    PATTERNS "node 0, launched for 2 tasks on 2 nodes, was called by node 1, launched for 2 tasks on 10 nodes"
             "osoe_notice: shmem_init on node 1 has waited [0-9]+ s for node 9, its child in the rendezvous tree, to call it on port [0-9]+"
    PROGRAM $<TARGET_FILE:shmem_test_job>)
  osoe_internode_label(shmem.job.secret.disagree.root-node-count
    shmem.job.secret.disagree.leaf-node-count shmem.job.secret.disagree.node-count)
endif()

# A start-up that goes wrong before the node has joined (the round-three
# review's R8; §6.2). Every PE checks every setting before its node joins, so a
# bad value given to all of them is refused by each, and one PE's difference from
# its lead is named by it and ends the node; the PE that refuses its own
# environment, which has no segment to say so in, tells its node by a tombstone
# (the lead) or in the lead's segment (any other); and what cannot be told - a PE
# that never calls shmem_init - is named by the notice of whoever waits. See
# refusal.sh; node.c, osoe_node_refusal_arm.
#
# OSOE_MAX_THREADS: 0 is refused by every PE, and 8 against the lead's 16 by the
# PE that has it - which names the difference - and ends the node.
osoe_refusal_test(shmem.start-up.max-threads.zero 2 0
  ENV OSOE_MAX_THREADS=0
  LAUNCH -n 2
  PATTERNS "osoe: PE 0: OSOE_MAX_THREADS='0' is not a number of threads from 1 to [0-9]+"
           "osoe: PE 1: OSOE_MAX_THREADS='0' is not a number of threads from 1 to [0-9]+"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
osoe_refusal_test(shmem.start-up.max-threads.differs 3 0
  LAUNCH -n 3 -e 2:OSOE_MAX_THREADS=8
  PATTERNS "osoe: PE 2: OSOE_MAX_THREADS differs between PEs: the local lead has [0-9]+, this PE has 8"
           "osoe: PE 0: another local PE failed during rendezvous"
           "osoe: PE 1: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_init>)

# The landing zone's size likewise, and a size that is not whole extents is
# refused by each PE, where only the lead's layout of it once was.
osoe_refusal_test(shmem.start-up.landing-bytes.differs 2 0
  LAUNCH -n 2 -e 1:OSOE_GET_LANDING_BYTES=8192
  PATTERNS "osoe: PE 1: OSOE_GET_LANDING_BYTES differs between PEs: the local lead has [0-9]+, this PE has 8192"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
osoe_refusal_test(shmem.start-up.landing-bytes.partial-extent 2 0
  ENV OSOE_GET_LANDING_BYTES=2000
  LAUNCH -n 2
  PATTERNS "osoe: PE 0: OSOE_GET_LANDING_BYTES=2000 is not a multiple of the landing zone's [0-9]+-byte extents"
           "osoe: PE 1: OSOE_GET_LANDING_BYTES=2000 is not a multiple of the landing zone's [0-9]+-byte extents"
  PROGRAM $<TARGET_FILE:shmem_test_init>)

# The AM layer's settings and the wire's, which a lead alone read before: the lead
# when it laid out the segment, and the wire's after the rendezvous and on a job
# of several nodes only, so that a job of one node accepted OSOE_WIRE_BYTES=abc
# without a word. Each PE refuses its own.
foreach(case_expect
    "am-chunk|OSOE_AM_CHUNK|abc|OSOE_AM_CHUNK='abc' is not a decimal byte count"
    "am-buffer|OSOE_AM_BUFFER|abc|OSOE_AM_BUFFER='abc' is not a decimal byte count"
    "am-buffer-budget|OSOE_AM_BUFFER_BUDGET|abc|OSOE_AM_BUFFER_BUDGET='abc' is not a decimal byte count"
    "am-credits|OSOE_AM_CREDITS|abc|OSOE_AM_CREDITS='abc' is not between [0-9]+ and [0-9]+"
    "am-hugepages|OSOE_AM_HUGEPAGES|2|OSOE_AM_HUGEPAGES='2' is not 0 or 1"
    "landing-bytes|OSOE_GET_LANDING_BYTES|abc|OSOE_GET_LANDING_BYTES='abc' is not a decimal byte count"
    "wire-bytes|OSOE_WIRE_BYTES|abc|OSOE_WIRE_BYTES='abc' is not a byte count between 64 and 1048576"
    "wire-unsent-bytes|OSOE_WIRE_UNSENT_BYTES|abc|OSOE_WIRE_UNSENT_BYTES='abc' is not a byte count: 0 to leave the system's own, or up to 1073741824"
    "wire-unsent-bytes-large|OSOE_WIRE_UNSENT_BYTES|2000000000|OSOE_WIRE_UNSENT_BYTES='2000000000' is more than 1073741824 bytes"
    "wire-zero-copy|OSOE_WIRE_ZERO_COPY|yes|OSOE_WIRE_ZERO_COPY=yes: on, to send connection buffers by reference"
    "wire-zero-copy-bytes|OSOE_WIRE_ZERO_COPY_BYTES|0|OSOE_WIRE_ZERO_COPY_BYTES='0' is not a byte count of at least 1"
    "progress-helpers|OSOE_PROGRESS_HELPERS|abc|OSOE_PROGRESS_HELPERS='abc' is not a number of threads from 0 to 64"
    "progress-helpers-many|OSOE_PROGRESS_HELPERS|65|OSOE_PROGRESS_HELPERS='65' is not a number of threads from 0 to 64")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 label)
  list(GET parts 1 variable)
  list(GET parts 2 value)
  list(GET parts 3 expect)
  osoe_refusal_test(shmem.start-up.${label}.invalid 2 0
    ENV ${variable}=${value}
    LAUNCH -n 2
    PATTERNS "osoe: PE 0: ${expect}" "osoe: PE 1: ${expect}"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
endforeach()

if(OSOE_INTERNODE)
  # A non-lead whose OSOE_AM_BUFFER made a larger connection area than its lead's
  # once waited for ever for the object to grow, which the lead sizes in one step.
  # It refuses at once, by name, and the job ends: the other PE of its node by the
  # node's abort, and the PEs of the other node as the lead they wait for ends.
  osoe_refusal_test(shmem.start-up.am-buffer.differs 4 0
    LAUNCH -n 4 -m 0,0,1,1 -e 1:OSOE_AM_BUFFER=524288
    PATTERNS "osoe: PE 1: connection area /osoe[.][0-9]+[.]0[.]0[.]conn[.]0 is [0-9]+ bytes, as the local lead's OSOE_AM_[*] settings made it; this PE's make it [0-9]+: every PE of a job must set OSOE_AM_[*] the same"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  # And the lead's the larger: the object is whole before the PE looks, and its
  # header says what it was made with.
  osoe_refusal_test(shmem.start-up.am-buffer.lead-larger 4 0
    LAUNCH -n 4 -m 0,0,1,1 -e 0:OSOE_AM_BUFFER=524288
    PATTERNS "osoe: PE 1: connection area /osoe[.][0-9]+[.]0[.]0[.]conn[.]0 was created with different AM sizing [(]buffer 524288 chunk [0-9]+ nodes 2, not [0-9]+/[0-9]+/2[)]: every PE of a job must set OSOE_AM_[*] the same"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  osoe_internode_label(shmem.start-up.am-buffer.differs shmem.start-up.am-buffer.lead-larger)
  # OSOE_PROGRESS_HELPERS differs between a node's PEs (v0.2 step 4, Part 3): the
  # node's own rendezvous refuses the PE that differs, naming both counts, where
  # the connection area would have refused it in the name of OSOE_AM_* (the
  # adversarial read of Part 1, N3). On both nodes, so that every rank ends; the
  # leads' count given too, so that a suite run with a count of its own keeps it.
  osoe_refusal_test(shmem.start-up.progress-helpers.differs 4 0
    LAUNCH -n 4 -m 0,0,1,1 -e 0:OSOE_PROGRESS_HELPERS=0 -e 1:OSOE_PROGRESS_HELPERS=2
           -e 2:OSOE_PROGRESS_HELPERS=0 -e 3:OSOE_PROGRESS_HELPERS=2
    PATTERNS "osoe: PE 1: OSOE_PROGRESS_HELPERS differs between PEs: the local lead has 0, this PE has 2"
             "osoe: PE 3: OSOE_PROGRESS_HELPERS differs between PEs: the local lead has 0, this PE has 2"
             "osoe: PE 0: another local PE failed during rendezvous"
             "osoe: PE 2: another local PE failed during rendezvous"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  osoe_internode_label(shmem.start-up.progress-helpers.differs)
endif()

# A PE whose own environment is bad refuses it before its node has joined, and
# its node-mates end with "another local PE failed" instead of waiting for it:
# a non-lead tells the lead's segment, and a lead - which has no segment yet -
# leaves a tombstone for the others to find.
# A start-up that ends in milliseconds says nothing of waiting: a notice is due
# only once a wait has lasted (the second pattern of each, with the hook at 5 s so
# that a machine that stalls for 2 does not make one due).
osoe_refusal_test(shmem.start-up.own-environment.non-lead 3 0
  ENV OSOE_TEST_NOTICE_SECONDS=5
  LAUNCH -n 3 -e 2:SHMEM_SYMMETRIC_SIZE=abc
  PATTERNS "osoe: PE 2: SHMEM_SYMMETRIC_SIZE='abc' is invalid"
           "!has waited [0-9]+ s"
           "osoe: PE 0: another local PE failed during rendezvous"
           "osoe: PE 1: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
osoe_refusal_test(shmem.start-up.own-environment.lead 3 0
  ENV OSOE_TEST_NOTICE_SECONDS=5
  LAUNCH -n 3 -e 0:SHMEM_SYMMETRIC_SIZE=abc
  PATTERNS "osoe: PE 0: SHMEM_SYMMETRIC_SIZE='abc' is invalid"
           "!has waited [0-9]+ s"
           "osoe: PE 1: another local PE failed during rendezvous"
           "osoe: PE 2: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_init>)

# A PE that cannot read its own identity - its job, step, node or local id - has
# no segment to tell and no node to tell it to: it ends, and the notice of the
# PEs that wait for it names it as it names one that never came (below).
osoe_refusal_test(shmem.start-up.own-identity.non-lead 2 1
  LAUNCH -n 2 -e 1:OSOE_LOCALID=x
  PATTERNS "osoe: PE 1: OSOE_LOCALID='x' is not a decimal number"
           "osoe: PE 0: shmem_init on node 0 has waited [0-9]+ s for local PEs to join the node [(]call shmem_init[)]: 1 of 2 have; still waiting for local id 1 [(]rank 1[)]"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
osoe_refusal_test(shmem.start-up.own-identity.lead 3 2
  LAUNCH -n 3 -e 0:OSOE_LOCALID=x
  PATTERNS "osoe: PE 0: OSOE_LOCALID='x' is not a decimal number"
           "osoe: PE 1: shmem_init on node 0 has waited [0-9]+ s for the local lead, rank 0, to create the node's control segment /osoe[.][0-9]+[.]0[.]0[.]0, which does not exist yet"
           "!osoe: PE 2: shmem_init on node"
  PROGRAM $<TARGET_FILE:shmem_test_init>)

# A PE that exits before shmem_init runs no library code and tells nobody. The
# node's other PEs wait for it - no wait gives up - and the notice of the wait
# says who: the lead names the local PEs that have not joined, by local id and
# rank, and says it again after twice the interval (the second pattern: a notice
# that came after 5 s of a wait whose first came at 2); and while the lead has
# made no segment to speak through, local id 1 names the lead, once for the node,
# not once a PE.
osoe_refusal_test(shmem.start-up.never-calls-init.non-lead 3 2
  SECONDS 10
  LAUNCH -n 3
  PATTERNS "osoe: PE 0: shmem_init on node 0 has waited [0-9]+ s for local PEs to join the node [(]call shmem_init[)]: 2 of 3 have; still waiting for local id 2 [(]rank 2[)]"
           "osoe: PE 0: shmem_init on node 0 has waited ([5-9]|1[0-9]) s for local PEs to join the node"
           "!osoe: PE 1: shmem_init on node"
  PROGRAM sh ${CMAKE_CURRENT_SOURCE_DIR}/exit-before-init.sh 2 $<TARGET_FILE:shmem_test_init>)
# A lead that ends after it has made the segment, while the node joins: the
# node's other PEs, which wait on its count and print no notice of their own,
# end by themselves naming it, where they were silent (the adversarial read of
# 2026-10-03). See lead-ends.sh.
osoe_refusal_test(shmem.start-up.lead-ends-in-join 3 0
  LAUNCH -n 3
  PATTERNS "osoe: PE 1: the local lead, rank 0 [(]pid [0-9]+[)], has ended while this PE waited for local PEs to join the node"
  PROGRAM sh ${CMAKE_CURRENT_SOURCE_DIR}/lead-ends.sh 3 2 $<TARGET_FILE:shmem_test_init>)
# A start by the ids of one that failed, its tombstone still standing: it runs,
# five times of five. See start-same-ids.sh.
add_test(NAME shmem.start-up.same-ids
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/start-same-ids.sh $<TARGET_FILE:osoe-launch>
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.start-up.same-ids PROPERTIES LABELS "tier2" TIMEOUT 150
  PASS_REGULAR_EXPRESSION "SAME IDS OK")
# The tombstone a refusing lead leaves, as the library makes it, kept by the
# launcher: osoe-stat shows it as one, and --gc removes it.
add_test(NAME shmem.start-up.tombstone-gc
  COMMAND sh -c "OSOE_KEEP_CONTROL_SEGMENT=1 \"$0\" -n 3 -t 20 -k -e 0:OSOE_MAX_THREADS=0 -- \"$1\" & L=$!; wait $L; \"$2\" /osoe.$L.0.0.0; \"$2\" --gc /osoe.$L.0.0.0"
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> $<TARGET_FILE:osoe-stat>)
set_tests_properties(shmem.start-up.tombstone-gc PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "a failed start's tombstone - .*/osoe[.][0-9]+[.]0[.]0[.]0: removed")
# Unless the job asks for it - OSOE_NO_PROGRESS_TIMEOUT, which is never by default -
# when the node ends by itself, naming the same. The lead and the PE that has
# joined wait alike, and the one whose limit comes first says it; the other ends
# with that or with "another local PE failed", whichever it sees first.
osoe_refusal_test(shmem.start-up.never-calls-init.no-progress-timeout 3 0
  ENV OSOE_NO_PROGRESS_TIMEOUT=3
  LAUNCH -n 3
  PATTERNS "osoe: PE [01]: no local PE has joined for OSOE_NO_PROGRESS_TIMEOUT seconds; missing local ids: 2 [(]rank 2[)]"
  PROGRAM sh ${CMAKE_CURRENT_SOURCE_DIR}/exit-before-init.sh 2 $<TARGET_FILE:shmem_test_init>)
osoe_refusal_test(shmem.start-up.never-calls-init.lead 3 2
  LAUNCH -n 3
  PATTERNS "osoe: PE 1: shmem_init on node 0 has waited [0-9]+ s for the local lead, rank 0, to create the node's control segment /osoe[.][0-9]+[.]0[.]0[.]0, which does not exist yet"
           "!osoe: PE 2: shmem_init on node"
  PROGRAM sh ${CMAKE_CURRENT_SOURCE_DIR}/exit-before-init.sh 0 $<TARGET_FILE:shmem_test_init>)

if(OSOE_INTERNODE)
  # A node that refuses leaves the other nodes waiting for it (a refusing node
  # telling its neighbours is not done), and the lead of each says for whom: here
  # the PEs of node 1 end naming the difference between them, or the lead's own
  # environment, and node 0's lead names node 1, the port it waits on and that
  # nothing has called it. Node 0's other PE waits for its lead, which says nothing
  # for the node twice. Both PEs of node 1 compare their sizes, and the one that
  # is told the other has failed first ends with that instead.
  osoe_refusal_test(shmem.start-up.node-refuses.size-mismatch 4 2
    LAUNCH -n 4 -m 0,0,1,1 -e 3:SHMEM_SYMMETRIC_SIZE=64M
    PATTERNS "osoe: PE [23]: SHMEM_SYMMETRIC_SIZE differs between PEs"
             "osoe: PE 0: shmem_init on node 0 has waited [0-9]+ s for node 1, its child in the rendezvous tree, to call it on port [0-9]+"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  osoe_refusal_test(shmem.start-up.node-refuses.own-environment-lead 4 2
    LAUNCH -n 4 -m 0,0,1,1 -e 2:SHMEM_SYMMETRIC_SIZE=abc
    PATTERNS "osoe: PE 2: SHMEM_SYMMETRIC_SIZE='abc' is invalid"
             "osoe: PE 3: another local PE failed during rendezvous"
             "osoe: PE 0: shmem_init on node 0 has waited [0-9]+ s for node 1, its child in the rendezvous tree, to call it on port [0-9]+"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  osoe_internode_label(shmem.start-up.node-refuses.size-mismatch
    shmem.start-up.node-refuses.own-environment-lead)
endif()

# The library against its floor (§1, §8.4): in each of seven pairs, 8-byte
# io_uring round trips on a raw connection between the job's two processes
# interleaved with the library's get, put and quiet and fetch-add, then a
# stream on each. The pins (test-floor.c) are checked in the optimized,
# uninstrumented build only, and only when asked: they are the project's
# regression guard, set on its own machines, so the project's runs configure
# with -DOSOE_TEST_PINS=ON and a stranger's `cmake --workflow` does not; the
# test then prints its ratios and says it checked none. The other presets run
# the paths and the invariants. Not under ThreadSanitizer, for shmem.internode's
# reason below: the lead's agent applies PE 0's puts to a heap whose owner last
# touched it before a barrier that runs through another process.
option(OSOE_TEST_PINS "check shmem.floor's performance pins, tier 3's included (in an optimized build): the project's regression guard, set on its own machines" OFF)
if(OSOE_INTERNODE)
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
    osoe_internode_label(shmem.floor)
    if(OSOE_TEST_PINS)
      set_tests_properties(shmem.floor PROPERTIES ENVIRONMENT "OSOE_TEST_FLOOR_PINS=1")
    endif()
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
  # internode: shmem_test_listener includes listener.c
  osoe_internode_label(shmem.listener.secret)

  # The progress loop between two logical nodes (tier 2): every layer at once.
  # The wire is far smaller than a segment and its split injection is on, so the
  # stream really arrives in pieces (§8.2). Sizes are pinned rather than
  # inherited, so the production defaults cannot make the suite slow or the test
  # vacuous.
  add_executable(shmem_test_progress test-progress.c
    ../src/progress.c ../src/collective-tree.c ../src/am.c ../src/connection.c ../src/connection-area.c ../src/am-configuration.c
    ../src/reply.c ../src/wire.c ../src/wire-settings.c ../src/ring.c ../src/listener.c ../src/shm-object.c)
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
  osoe_internode_label(shmem.progress shmem.progress.forged-get-reply
    shmem.progress.forged-get-reply-bounds)
  # A node that is ending excuses a peer's reset and sends nothing more on the
  # failed wire (the second adversarial read of Part 3, S1;
  # abort_owners_model.py's reset_stream): node 1's lead resets its data wire
  # and ends while node 0's lead, its node ending, has a send's completion still
  # to collect, which node 0 reaps in one pass with the reset. Offering that
  # segment again was send_from's fatal; without the node excuse the reset is
  # one too.
  add_test(NAME shmem.progress.reset-while-ending
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
            OSOE_TEST_FORGE=reset-while-ending
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 20 --
            $<TARGET_FILE:shmem_test_progress>)
  set_tests_properties(shmem.progress.reset-while-ending PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "RESET WHILE ENDING OK" FAIL_REGULAR_EXPRESSION "osoe_fatal")
  osoe_internode_label(shmem.progress.reset-while-ending)
  # The planes kept apart (the progress helpers proposal's §3.1): node 0's lead
  # sends a barrier arrival on its data wire, or a get reply on its control
  # wire, and node 1 must refuse each by name - the get reply before its
  # landing is looked up, which ended the job first as a reply nobody asked for
  # until the adversarial read of Part 2.
  foreach(forge barrier-on-data get-reply-on-control)
    if(forge STREQUAL "barrier-on-data")
      set(refusal "osoe_fatal: node 1: a barrier arrival from node 0 arrived on its data wire")
    else()
      set(refusal "osoe_fatal: node 1: a get reply from node 0 arrived on its control wire")
    endif()
    add_test(NAME shmem.progress.forged-${forge}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20260919
              OSOE_TEST_FORGE=${forge}
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 20 --
              $<TARGET_FILE:shmem_test_progress>)
    set_tests_properties(shmem.progress.forged-${forge} PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "${refusal}")
    osoe_internode_label(shmem.progress.forged-${forge})
  endforeach()

  # Owners at a helper count above none, every owner of a node turned by its
  # one thread (test-owners.c): put, get and fetching AMO between every two of 5
  # to 9 nodes at 2 and 3 helpers, where a wire's index and its peer slot
  # differ, each checked to land where its sender wrote and counted in its own
  # connection's record (the adversarial read of v0.2 step 4's Part 1, S1).
  add_executable(shmem_test_owners test-owners.c
    ../src/progress.c ../src/collective-tree.c ../src/am.c ../src/connection.c ../src/connection-area.c ../src/am-configuration.c
    ../src/reply.c ../src/wire.c ../src/wire-settings.c ../src/ring.c ../src/listener.c ../src/bootstrap.c
    ../src/shm-object.c)
  target_include_directories(shmem_test_owners PRIVATE ../src)
  target_link_libraries(shmem_test_owners PRIVATE osoe_base rt PkgConfig::LIBURING)
  osoe_warnings(shmem_test_owners)
  foreach(case 2:0,1,2,3,4 3:0,1,2,3,4 2:0,1,2,3,4,5,6,7,8 3:0,1,2,3,4,5,6,7,8)
    string(REPLACE ":" ";" parts "${case}")
    list(GET parts 0 helpers)
    list(GET parts 1 map)
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    add_test(NAME shmem.owners.H${helpers}.N${count}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_HELPERS=${helpers}
              OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=3000 OSOE_WIRE_SEED=20261008
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
              $<TARGET_FILE:shmem_test_owners>)
    set_tests_properties(shmem.owners.H${helpers}.N${count} PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_internode_label(shmem.owners.H${helpers}.N${count})
  endforeach()

  # The connection area across the PEs of a node (tier 2). The local lead creates
  # it and the others open it, which is §3.12's arrangement. Not an
  # osoe_tier2_test: it does not call shmem_init, testing the area on its own
  # below the library, and it takes its identity from the launcher environment.
  add_executable(shmem_test_connection_area test-connection-area.c ../src/connection-area.c ../src/am-configuration.c ../src/connection.c
    ../src/shm-object.c)
  target_include_directories(shmem_test_connection_area PRIVATE ../src)
  target_link_libraries(shmem_test_connection_area PRIVATE osoe_base rt)
  osoe_warnings(shmem_test_connection_area)
  foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
    add_test(NAME shmem.connection-area.L${count}
      COMMAND osoe-launch -n ${count} -t 60 -- $<TARGET_FILE:shmem_test_connection_area>)
    set_tests_properties(shmem.connection-area.L${count} PROPERTIES LABELS "tier2" TIMEOUT 90)
    # internode: compiles connection-area.c and connection.c
    osoe_internode_label(shmem.connection-area.L${count})
  endforeach()
endif()

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
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.multinode.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_multinode>)
  set_tests_properties(shmem.multinode.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
  if(nodes GREATER 1)
    osoe_internode_label(shmem.multinode.${label})
    # The zero-copy send (wire-settings.h) on every send of 64 bytes or more:
    # a connection buffer lent to the kernel until its notifications have come.
    # Over loopback the kernel copies anyway, but the notifications, the lent
    # buffers and their release at the last one run as they would on a NIC. The
    # deadlock detector is off, so that a wait the engine strands is the
    # launcher's timeout rather than a second's rescue by the detector's sweep
    # (the fourth adversarial read of v0.2 step 4, F1), and the test fails if no
    # send went by reference (S1).
    add_test(NAME shmem.multinode.zero-copy.${label}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
              OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=64 OSOE_DEADLOCK_SECONDS=0
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
              $<TARGET_FILE:shmem_test_multinode>)
    set_tests_properties(shmem.multinode.zero-copy.${label} PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_internode_label(shmem.multinode.zero-copy.${label})
  endif()
endforeach()

if(OSOE_INTERNODE)
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
      osoe_internode_label(shmem.tree.k${fanout}.n${nodes}.p${count})
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
  osoe_internode_label(shmem.work-discovery)

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
    osoe_internode_label(shmem.multinode.force-remote.${label})
  endforeach()
endif()

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
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.internode.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_AM_CREDITS=2
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_internode>)
  set_tests_properties(shmem.internode.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
  if(nodes GREATER 1)
    osoe_internode_label(shmem.internode.${label})
    # The zero-copy send, as shmem.multinode.zero-copy.* runs it.
    add_test(NAME shmem.internode.zero-copy.${label}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
              OSOE_AM_CREDITS=2
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
              OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=64 OSOE_DEADLOCK_SECONDS=0
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
              $<TARGET_FILE:shmem_test_internode>)
    set_tests_properties(shmem.internode.zero-copy.${label} PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_internode_label(shmem.internode.zero-copy.${label})
  endif()
endforeach()
# The zero-copy send refused for want of locked memory (the review of v0.2
# step 4's merge point, portability P1; ring.c): every PE lowers its own
# locked-memory limit to nothing once its rings are made
# (OSOE_TEST_LOCKED_MEMORY=0, locked-memory.h), so that every send by reference
# after shmem_init is refused, on any kernel and whatever else the user holds -
# a limit set before the job would have to leave room for rings, which Linux
# 6.14 and later charge too, and none for the sends. Each refused send goes
# again by copy, the job ends well, and each process says so once, naming its
# limit - where the job once ended "Cannot allocate memory" on a connection. A
# wire that went on lending after a refusal still ends this job well - a region
# owing the refused send's notification makes the next send copy - so
# shmem.wire.zero-copy-refused, which counts the refusals, is the test of that.
# Skipped where a process may lock memory whatever its
# limit (CAP_IPC_LOCK, which io_uring does not charge). Not under
# ThreadSanitizer, as this program's other multi-node maps are not (above).
if(OSOE_INTERNODE AND NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_test(NAME shmem.internode.zero-copy-refused.0-1-2
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
            OSOE_AM_CREDITS=2
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            OSOE_WIRE_ZERO_COPY=on OSOE_WIRE_ZERO_COPY_BYTES=64 OSOE_DEADLOCK_SECONDS=0
            OSOE_TEST_LOCKED_MEMORY=0
            $<TARGET_FILE:osoe-launch> -n 3 -m 0,1,2 -t 60 --
            $<TARGET_FILE:shmem_test_internode>)
  set_tests_properties(shmem.internode.zero-copy-refused.0-1-2 PROPERTIES LABELS "tier2"
    TIMEOUT 90 PASS_REGULAR_EXPRESSION "refused for want of locked memory"
    FAIL_REGULAR_EXPRESSION
      "Cannot allocate memory;exited with status [1-9];killed by signal;timeout after;left behind"
    SKIP_REGULAR_EXPRESSION "SKIPPED: a PE may lock memory")
  osoe_internode_label(shmem.internode.zero-copy-refused.0-1-2)
endif()
# The same through the bootstrap's Slurm path (§6.1): osoe-launch -s names the
# launcher's variables as srun does, SLURM_ in place of OSOE_. One node, one PE
# a node, the block map, and the cyclic one, whose ranks are not contiguous per
# node - the distribution §6.1 says the environment cannot describe, and the
# rendezvous must assemble from each node's SLURM_GTIDS.
if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
  set(osoe_slurm_maps 0,0,0,0)
else()
  set(osoe_slurm_maps 0,0,0,0 0,1,2 0,0,1,1,2,2 0,1,2,0,1,0)
endif()
foreach(map IN LISTS osoe_slurm_maps)
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  string(REPLACE "," "-" label "${map}")
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.slurm.internode.${label}
    COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_PROCID OSOE_AM_CHUNK=1024
            OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2
            OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
            $<TARGET_FILE:osoe-launch> -s -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_internode>)
  set_tests_properties(shmem.slurm.internode.${label} PROPERTIES LABELS "tier2" TIMEOUT 90)
  if(nodes GREATER 1)
    osoe_internode_label(shmem.slurm.internode.${label})
  endif()
endforeach()
if(OSOE_INTERNODE)
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
    osoe_internode_label(shmem.internode.force-remote.${label})
  endforeach()
  endif()
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
foreach(name rma amo amo-types amo-nbi wait wait-many signal team malloc barrier)
  add_test(NAME shmem.heap-shift.${name}.L4
    COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_SHIFT=2m
            $<TARGET_FILE:osoe-launch> -n 4 -t 60 -- $<TARGET_FILE:shmem_test_${name}>)
  set_tests_properties(shmem.heap-shift.${name}.L4 PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()
if(OSOE_INTERNODE)
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
      osoe_internode_label(shmem.heap-shift.internode.${label})
    endforeach()
  endif()
endif()
# OSOE_HEAP_SHIFT is a whole number of pages, and anything else is refused by name.
add_test(NAME shmem.heap-shift.invalid
  COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_SHIFT=100
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_rma>)
set_tests_properties(shmem.heap-shift.invalid PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_HEAP_SHIFT='100' is not a whole number of")
# So is an OSOE_HEAP_BASE off a page boundary, which would otherwise fail to map
# and be reported as a base that is not free. 2 KiB past a boundary is off one
# at every page size.
add_test(NAME shmem.refuse.heap_base_unaligned
  COMMAND ${CMAKE_COMMAND} -E env OSOE_HEAP_BASE=0x100000000800
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_rma>)
set_tests_properties(shmem.refuse.heap_base_unaligned PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_HEAP_BASE='0x100000000800' is not a multiple of the")

if(OSOE_INTERNODE)
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
  osoe_internode_label(shmem.differential)
  endif()
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
# reader, so it can read the control segment's metrics by name. A map of more
# than one node is the internode label's, and the single-node build registers
# none (cmake/OsoeInternode.cmake); a map of one node runs in both builds. The
# rest of the arguments are the job's environment, but SECONDS n, the
# launcher's limit, 60 unless given, and ctest's 30 more.
function(osoe_tier2_map_test name map)
  cmake_parse_arguments(PARSE_ARGV 2 test "" "SECONDS" "")
  if(NOT test_SECONDS)
    set(test_SECONDS 60)
  endif()
  math(EXPR timeout "${test_SECONDS} + 30")
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    return()
  endif()
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
    COMMAND ${CMAKE_COMMAND} -E env ${test_UNPARSED_ARGUMENTS}
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t ${test_SECONDS} --
            $<TARGET_FILE:shmem_test_${name}>)
  set_tests_properties(shmem.${name}.${label} PROPERTIES LABELS "tier2" TIMEOUT ${timeout})
  if(nodes GREATER 1)
    osoe_internode_label(shmem.${name}.${label})
  endif()
endfunction()

# Link a test program as oshcc links one (design §4.3): -fno-pie, -no-pie and
# osoe's linker script, so that its global and static variables are symmetric.
function(osoe_statics_program target)
  set(script ${PROJECT_SOURCE_DIR}/tools/wrapper/osoe-statics.ld)
  target_compile_options(${target} PRIVATE -fno-pie)
  target_link_options(${target} PRIVATE -no-pie "LINKER:-T,${script}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${script})
endfunction()

# A quiesced pair falls silent (§3.4; review round two, C1). Before slice 2's
# acknowledgement protocol: 1,320 header-only segments per node in the idle
# half second.
osoe_tier2_map_test(ack-echo 0,1)

# shmem_init raises the soft limits on open files and on locked memory to the
# hard ones (init.c; the author's decision of 2026-10-11, after the ninth
# review's P9-1), each lowered by the program before its init: on one node, in
# both builds, and across two, whose rings a kernel from 6.14 on would refuse at
# the locked-memory limit the program lowered to nothing.
osoe_tier2_map_test(limits 0,0)
osoe_tier2_map_test(limits 0,1)
foreach(map IN ITEMS 0-0 0-1)
  if(TEST shmem.limits.${map})
    set_tests_properties(shmem.limits.${map} PROPERTIES
      PASS_REGULAR_EXPRESSION "LIMITS RAISED OK"
      FAIL_REGULAR_EXPRESSION "CHECK[(]|after shmem_init")
  endif()
endforeach()

# Progress helpers (v0.2 step 4, Part 3; owner-map.h). A PE's wake reaches the
# owner of its wire: each put of PE 0's, made once the owner has raised the flag
# it raises before it sleeps, claims that owner's wake and wakes the helper the
# map names for its peer, and no other owner of the node (test-helper-wake.c;
# the wake count is the owner's, layout 36; the flag read where PE 0's process
# maps the connection area, base/cacheline.h giving the line). The deadlock
# detector, whose requests nudge every helper, is off. Not under
# ThreadSanitizer, as no map of more than one node is: the helpers land the
# puts in heaps their PEs' own threads touch, ordered through another process,
# and the preset runs at no helpers (the second adversarial read of Part 3, S2).
# Not run alone any more: waiting for the flag, both runs hold beside four busy
# loops on four cores, 20 runs of 20 in the release build and under
# AddressSanitizer, where counting wakes without it failed 8 of 20 in the first
# and 9 of 20 in the second (docs/decisions/2026-10-10-helper-wake.md). The same
# at no helpers, where the lead's agent is owner 0 and holds every wire: a spin
# before its idle wait shows as a helper's does (scale S1, the agent's half) -
# a spin longer than an acknowledgement's round trip surely, a shorter one alone
# but less surely under load.
if(OSOE_INTERNODE AND NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(helper-wake 0,1,2,3 OSOE_PROGRESS_HELPERS=2 OSOE_DEADLOCK_SECONDS=0)
  target_include_directories(shmem_test_helper-wake PRIVATE ${PROJECT_SOURCE_DIR})
  add_test(NAME shmem.helper-wake.agent.0-1-2-3
    COMMAND ${CMAKE_COMMAND} -E env OSOE_PROGRESS_HELPERS=0 OSOE_DEADLOCK_SECONDS=0
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 60 --
            $<TARGET_FILE:shmem_test_helper-wake>)
  set_tests_properties(shmem.helper-wake.agent.0-1-2-3 PROPERTIES LABELS "tier2" TIMEOUT 90)
  osoe_internode_label(shmem.helper-wake.agent.0-1-2-3)
endif()

# Keepalive off by default where every node has a loopback address, and on as
# OSOE_KEEPALIVE sets it there (§3.15; the review of v0.2 step 4's merge point,
# correctness Q2-1; test-keepalive.c): each node's lead reads SO_KEEPALIVE on
# its own node-pair sockets, and the silent-peer check, which the same setting
# turns off and on, says its choice under SHMEM_DEBUG - the opposite line fails
# the test. tier3.pi.keepalive.default-on.0-1 holds the default between nodes
# with addresses of their own.
if(OSOE_INTERNODE)
  add_executable(shmem_test_keepalive test-keepalive.c)
  target_link_libraries(shmem_test_keepalive PRIVATE osoe)
  osoe_warnings(shmem_test_keepalive)
  add_test(NAME shmem.keepalive.loopback-off.0-1
    COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_KEEPALIVE SHMEM_DEBUG=1
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 --
            $<TARGET_FILE:shmem_test_keepalive> off)
  add_test(NAME shmem.keepalive.set-on.0-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_KEEPALIVE=60,10,6 SHMEM_DEBUG=1
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 --
            $<TARGET_FILE:shmem_test_keepalive> on)
  set_tests_properties(shmem.keepalive.loopback-off.0-1 PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "KEEPALIVE OFF OK"
    FAIL_REGULAR_EXPRESSION "probes its connections with TCP keepalive")
  set_tests_properties(shmem.keepalive.set-on.0-1 PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "KEEPALIVE ON OK"
    FAIL_REGULAR_EXPRESSION "makes no silent-peer check")
  osoe_internode_label(shmem.keepalive.loopback-off.0-1)
  osoe_internode_label(shmem.keepalive.set-on.0-1)
endif()
# A helper that never answers its stop - OSOE_TEST_DEAF_HELPER, the defect
# helper_request_model.py's `deaf` case stands for - ends the job at
# shmem_finalize naming it, found by its turns: nothing waits for time to pass.
if(OSOE_INTERNODE)
  add_test(NAME shmem.helpers.deaf.0-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_PROGRESS_HELPERS=2 OSOE_TEST_DEAF_HELPER=1
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_barrier>)
  set_tests_properties(shmem.helpers.deaf.0-1 PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION
      "progress helper 1 of node [01] has gone round its loop [0-9]+ times since it was asked to stop, and has not answered")
  osoe_internode_label(shmem.helpers.deaf.0-1)
  # An ending node's agent waits for its helpers' abortive goodbyes before its
  # process ends (abort_owners_model.py's no_wait): under OSOE_TEST_ABORT_DELAY_MS
  # a helper begins its abort half a second late, and a peer acts on an abortive
  # goodbye half a second late, so that only the data wire's own goodbye, ahead
  # of its end, keeps the peer's helper from taking that end for a death - here a
  # non-lead's fatal, which its lead's goodbyes carry (global-exit.sh's lost
  # goodbyes and strays).
  add_test(NAME shmem.helpers.abort-wait.0-0-1-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_PROGRESS_HELPERS=2 OSOE_TEST_ABORT_DELAY_MS=500
            sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 fatal 1 1 compute)
  set_tests_properties(shmem.helpers.abort-wait.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  osoe_internode_label(shmem.helpers.abort-wait.0-0-1-1)
  # A node that is ending excuses a peer's end, and sends nothing more on a
  # connection that failed (abort_owners_model.py's reset_stream; the second
  # adversarial read of Part 3, S1): node 0 ends by a non-lead's global exit, its
  # helpers half a second behind it, while node 1 streams to it; node 1's lead
  # dies of a fatal of its own 150 ms after the exit, and node 0's helpers read
  # its connections' end - a reset where input was unread - before their own
  # abort. Without the excuse, or with a send on the failed connection, node 0's
  # helper raises a fatal and its agent waits out the grace.
  add_test(NAME shmem.helpers.reset-stream.0-0-1-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_PROGRESS_HELPERS=2 OSOE_TEST_ABORT_DELAY_MS=500
            sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 1 reset-stream)
  set_tests_properties(shmem.helpers.reset-stream.0-0-1-1 PROPERTIES LABELS "tier2"
    TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  osoe_internode_label(shmem.helpers.reset-stream.0-0-1-1)
endif()

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

if(OSOE_INTERNODE)
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
  osoe_internode_label(shmem.unswept-wait)

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
  osoe_internode_label(shmem.hang-walk)

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
  # The target's pool is, per connection, C and one for the deadlock detector,
  # plus (k + 1) x OSOE_TEAM_BLOCKS for the tree's forwards, node-wide (§3.8):
  # with C = 1, k = 2 and the smallest pool of team blocks the variable allows, 8,
  # that is 1 + 1 + 3 x 8 = 26, so twenty-seven origin PEs each with a blocking
  # get outstanding overflow it, and thirty-one leave a margin. Slice 3 made the
  # forward term node-wide - at the default k = 4 it was 16, and the seven origin
  # PEs this test had until then stopped reaching it - phase 1 step 7's second
  # slice made it count every team number, where it had counted the three
  # predefined teams and eleven origins overflowed it, and the team redesign's
  # step D every block of the node's pool, a node holding more teams than one
  # PE's numbers, where fifteen origins overflowed 1 + 1 + 3 x 4.
  add_test(NAME shmem.reply-pool.overflow
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_POOL=overflow SHMEM_SYMMETRIC_SIZE=16m
            OSOE_COLLECTIVE_FANOUT=2
            $<TARGET_FILE:osoe-launch> -n 32
            -m 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 -t 30
            -e 31:OSOE_AM_CREDITS=1 -e 31:OSOE_TEAM_BLOCKS=8
            -- $<TARGET_FILE:shmem_test_reply-pool>)
  set_tests_properties(shmem.reply-pool.overflow PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "reply descriptor pool is empty.*request from node 0")
  osoe_internode_label(shmem.reply-pool.overflow)

  # A quiet waits as long as its peer does (§3.15; review round two, D3): node 1
  # stopped for 3 s, then continued, and the quiet completes with the no-progress
  # limit unset; with it at 10 s and node 1 stopped for 18, the quiet ends the job
  # naming the connection. The slice-2 commit records a run stopped for 65 s, past
  # the old 60 s bound. The limit was 1 s until a Pi took longer than that to
  # connect a second logical node under ctest -j4, and the limit fired at start-up
  # instead (docs/pi-cluster.md, 2026-09-24); then 5 s, until it fired so on a Pi
  # node beside tier 2's largest jobs (the ninth review's Pi rehearsal).
  osoe_tier2_map_test(stopped-node 0,0,1 OSOE_TEST_STOP_MS=3000)
  set_tests_properties(shmem.stopped-node.0-0-1 PROPERTIES
    PASS_REGULAR_EXPRESSION "quiet completed after node 1 was stopped")
  add_test(NAME shmem.stopped-node.no-progress
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STOP_MS=18000 OSOE_NO_PROGRESS_TIMEOUT=10
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
  osoe_internode_label(shmem.stopped-node.no-progress shmem.stopped-node.keepalive)
  # A stopped node whose receive buffer is full (phase 1 release review, K5): the state
  # the test above never reaches, and the one the silent-peer check's condition
  # exists for - the kernel is asked how long its peer has acknowledged nothing
  # only while something sent is unacknowledged (wire.c, osoe_wire_silent_ms). 64 MiB
  # put toward node 1 while it is stopped shuts its window, which is probed with
  # nothing outstanding. Without the condition the job ends at about 12 s, naming a
  # node that was never gone, and every other stopped-node test still passes. The
  # stop is 15 s: 12 s let the removal through and 13 s did not, so that leaves
  # 2.8 s over the time the removal takes (the test's header has the arithmetic).
  # In every preset, as the stopped-node tests are. Under ThreadSanitizer with
  # helpers its check of the 64 MiB a helper landed is eight million racy reads
  # in the tool's slow path, 117 s at two helpers (docs/decisions/2026-10-08-helpers.md),
  # past the launcher's minute: given five, as ctest must judge every test there
  # (the ninth review, C9-1).
  set(stopped_bulk_seconds 60)
  if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
    set(stopped_bulk_seconds 300)
  endif()
  osoe_tier2_map_test(stopped-bulk 0,0,1 SECONDS ${stopped_bulk_seconds} OSOE_TEST_STOP_MS=15000
    OSOE_KEEPALIVE=1,1,3 SHMEM_SYMMETRIC_SIZE=128m)
  set_tests_properties(shmem.stopped-bulk.0-0-1 PROPERTIES
    PASS_REGULAR_EXPRESSION "STOPPED BULK OK"
    FAIL_REGULAR_EXPRESSION "stopped answering|CHECK[(]|osoe-launch: (rank|timeout)")

  # The other half of D-6 (diagnosability review): a blocking get waits for one
  # PE's reply, not a whole node's queue, so its no-progress fatal must name that
  # PE and not only the node. Before this fix it named neither. Ten seconds, like
  # shmem.stopped-node.no-progress's, because the limit covers start-up too.
  osoe_tier2_map_test(get-no-progress 0,1 OSOE_NO_PROGRESS_TIMEOUT=10)
  set_tests_properties(shmem.get-no-progress.0-1 PROPERTIES
    PASS_REGULAR_EXPRESSION "shmem_get: no progress toward node 1 [(]PE 1[)] for [0-9]+ s"
    FAIL_REGULAR_EXPRESSION "GET RETURNED")
  # And a get of more than a piece, whose pieces go out together (remote_get):
  # three pieces of a default chunk's payload, which the landing zone holds, wait
  # for their bytes; twenty wait for room at the fifth, the zone holding four.
  # Each wait must name node and PE as a piece's reply does - which both named
  # neither before the adversarial read of v0.2's blocking gets (finding 2) -
  # and the expression names the wait, so that each test pins its own.
  foreach(case IN ITEMS "3;150000;the bytes of its get's pieces"
                        "20;1300000;the landing zone's released extents")
    list(GET case 0 pieces)
    list(GET case 1 bytes)
    list(GET case 2 watching)
    add_test(NAME shmem.get-no-progress.pieces-${pieces}.0-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_NO_PROGRESS_TIMEOUT=10 OSOE_TEST_GET_BYTES=${bytes}
              $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 --
              $<TARGET_FILE:shmem_test_get-no-progress>)
    set_tests_properties(shmem.get-no-progress.pieces-${pieces}.0-1 PROPERTIES
      LABELS "tier2" TIMEOUT 90
      PASS_REGULAR_EXPRESSION
        "shmem_get: no progress toward node 1 [(]PE 1[)] for [0-9]+ s [(]OSOE_NO_PROGRESS_TIMEOUT[)]: ${watching} has stayed at"
      FAIL_REGULAR_EXPRESSION "GET RETURNED")
    osoe_internode_label(shmem.get-no-progress.pieces-${pieces}.0-1)
  endforeach()
  # And a get whose room another node holds (the ninth review, Q5-2): PE 0's
  # zone, set to 64 KiB, filled by a nonblocking get from PE 2, which stops, and
  # PE 0's blocking get from PE 1, live, waiting for room. Its wait names no
  # node, whether its pieces go one at a time (8 bytes) or together (three
  # pieces), where both named node 1 before. The routine must be the blocking
  # get's: had the nonblocking get waited itself, it would say its own.
  foreach(bytes IN ITEMS 8 150000)
    add_test(NAME shmem.get-no-progress.zone-held.${bytes}.0-1-2
      COMMAND ${CMAKE_COMMAND} -E env OSOE_NO_PROGRESS_TIMEOUT=10 OSOE_TEST_GET_BYTES=${bytes}
              OSOE_GET_LANDING_BYTES=65536
              $<TARGET_FILE:osoe-launch> -n 3 -m 0,1,2 -t 60 --
              $<TARGET_FILE:shmem_test_get-no-progress>)
    set_tests_properties(shmem.get-no-progress.zone-held.${bytes}.0-1-2 PROPERTIES
      LABELS "tier2" TIMEOUT 90
      PASS_REGULAR_EXPRESSION
        "shmem_get: no progress for [0-9]+ s [(]OSOE_NO_PROGRESS_TIMEOUT[)]: the landing zone's released extents has stayed at"
      FAIL_REGULAR_EXPRESSION "GET RETURNED|toward node")
    osoe_internode_label(shmem.get-no-progress.zone-held.${bytes}.0-1-2)
  endforeach()
endif()

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
# D13): every PE dies of SIGKILL or SIGTERM after init, and the launcher must
# find nothing left behind. The kept variant leaves the control segments on
# purpose, and osoe-stat --gc must then remove them. On one node in both builds
# (v0.2 step 1's adversarial read: the single-node build had no test of it),
# and on two below.
add_executable(shmem_test_kill-after-init test-kill-after-init.c)
target_link_libraries(shmem_test_kill-after-init PRIVATE osoe)
osoe_warnings(shmem_test_kill-after-init)
foreach(sig KILL TERM)
  add_test(NAME shmem.kill-after-init.${sig}.one-node
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_SIGNAL=${sig}
            $<TARGET_FILE:osoe-launch> -n 4 -t 30 -k --
            $<TARGET_FILE:shmem_test_kill-after-init>)
  set_tests_properties(shmem.kill-after-init.${sig}.one-node PROPERTIES LABELS "tier2"
    TIMEOUT 60 PASS_REGULAR_EXPRESSION "killed by signal"
    FAIL_REGULAR_EXPRESSION "left behind")
endforeach()
add_test(NAME shmem.kill-after-init.kept.one-node
  COMMAND sh -c "OSOE_KEEP_CONTROL_SEGMENT=1 OSOE_TEST_SIGNAL=KILL \"$0\" -n 4 -t 30 -k -- \"$1\" & L=$!; wait $L; \"$2\" --gc /osoe.$L.0.0.0"
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_kill-after-init>
          $<TARGET_FILE:osoe-stat>)
set_tests_properties(shmem.kill-after-init.kept.one-node PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "kept for post-mortem reading.*removed"
  FAIL_REGULAR_EXPRESSION "left behind")

# A job never outlives its launcher: SIGTERM and SIGINT are forwarded, and a
# SIGKILLed launcher takes its PEs with it (launch.c; review round two,
# omissions 6). See launcher-signals.sh. On one node in both builds - the
# launcher is installed, the desktop's way to run a job - and on two below.
add_executable(shmem_test_sleeper sleeper.c)
target_link_libraries(shmem_test_sleeper PRIVATE osoe)
osoe_warnings(shmem_test_sleeper)
set(osoe_launcher_signals_term
  "launcher exited 143.*escalated to SIGKILL: 0.*PEs still running: 0.*with a name: 0")
set(osoe_launcher_signals_int
  "launcher exited 130.*escalated to SIGKILL: 0.*PEs still running: 0.*with a name: 0")
set(osoe_launcher_signals_kill "PEs still running: 0.*with a name: 0")
foreach(mode term int kill)
  add_test(NAME shmem.launcher-signals.${mode}.one-node
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/launcher-signals.sh
            $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_sleeper> ${mode} one-node)
  set_tests_properties(shmem.launcher-signals.${mode}.one-node PROPERTIES LABELS "tier2"
    TIMEOUT 60 PASS_REGULAR_EXPRESSION "${osoe_launcher_signals_${mode}}")
endforeach()

# internode: each of these launches a job of several nodes, here or in its script
if(OSOE_INTERNODE)
  foreach(sig KILL TERM)
    add_test(NAME shmem.kill-after-init.${sig}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_SIGNAL=${sig}
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -k --
              $<TARGET_FILE:shmem_test_kill-after-init>)
    set_tests_properties(shmem.kill-after-init.${sig} PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "killed by signal"
      FAIL_REGULAR_EXPRESSION "left behind")
    osoe_internode_label(shmem.kill-after-init.${sig})
  endforeach()
  # --gc on this job's two segments alone - the job id is the launcher's pid -
  # rather than every dead object of the user on the machine (the adversarial
  # read of 2026-10-02).
  add_test(NAME shmem.kill-after-init.kept
    COMMAND sh -c "OSOE_KEEP_CONTROL_SEGMENT=1 OSOE_TEST_SIGNAL=KILL \"$0\" -n 4 -m 0,0,1,1 -t 30 -k -- \"$1\" & L=$!; wait $L; \"$2\" --gc /osoe.$L.0.0.0 /osoe.$L.0.1.0"
            $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_kill-after-init>
            $<TARGET_FILE:osoe-stat>)
  set_tests_properties(shmem.kill-after-init.kept PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "kept for post-mortem reading.*removed"
    FAIL_REGULAR_EXPRESSION "left behind")
  osoe_internode_label(shmem.kill-after-init.kept)

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
  osoe_internode_label(shmem.leftovers.reclaim shmem.leftovers.live)

  # A start that never finishes leaves its node's job tables and connection area
  # in /dev/shm - under srun nothing else removes them (the round-three review's
  # R5a) - and osoe-stat --gc removes them once nothing uses them, and not before.
  # One PE stops inside init after its node has joined, the start is cancelled,
  # and the job's names are counted before and after. See start-leftovers.sh.
  add_executable(shmem_test_stalled-start test-stalled-start.c)
  target_link_libraries(shmem_test_stalled-start PRIVATE osoe)
  osoe_warnings(shmem_test_stalled-start)
  add_test(NAME shmem.leftovers.gc
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/start-leftovers.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_stalled-start> $<TARGET_FILE:osoe-stat>)
  set_tests_properties(shmem.leftovers.gc PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "START LEFTOVERS OK")
  osoe_internode_label(shmem.leftovers.gc)
  # And a start by the same ids, with no --gc between, takes none of them: its
  # lead replaces both and its PEs take only what their lead made - before, a
  # non-lead took the leftovers and the start ended as a false deadlock, or hung
  # (the release fixes' review, correctness F1). See restart-leftovers.sh.
  # The program it starts again is test-internode.c, whose puts and quiets use the
  # connection area; under tsan, test-multinode.c's barriers, since tsan reads the
  # agent writing a heap a local thread holds as a race (shmem.internode.*, above).
  if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
    set(osoe_restart_program shmem_test_multinode)
  else()
    set(osoe_restart_program shmem_test_internode)
  endif()
  add_test(NAME shmem.leftovers.restart
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/restart-leftovers.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_stalled-start> $<TARGET_FILE:${osoe_restart_program}>)
  set_tests_properties(shmem.leftovers.restart PROPERTIES LABELS "tier2" TIMEOUT 120
    PASS_REGULAR_EXPRESSION "RESTART LEFTOVERS OK")
  osoe_internode_label(shmem.leftovers.restart)
  # And with node 0's lead of the second start holding back making its
  # connection area until its node-mate has its job tables (slow-create.c): the
  # node-mate meets the leftover area, the fix's other half, which the plain case
  # does not reach - its lead nearly always replaces the area first. Not under
  # the sanitizers, as slow-message.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=")
    add_library(slow-create SHARED slow-create.c)
    target_link_libraries(slow-create PRIVATE ${CMAKE_DL_LIBS})
    osoe_warnings(slow-create)
    add_test(NAME shmem.leftovers.restart.slow-lead
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/restart-leftovers.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_stalled-start> $<TARGET_FILE:shmem_test_internode>
              $<TARGET_FILE:slow-create>)
    set_tests_properties(shmem.leftovers.restart.slow-lead PROPERTIES LABELS "tier2"
      TIMEOUT 120 PASS_REGULAR_EXPRESSION "RESTART LEFTOVERS OK")
    # And the second start on three nodes, its area larger than the two-node
    # leftover: the node-mate must not take the leftover's size for its lead's
    # settings.
    add_test(NAME shmem.leftovers.restart.other-size
      COMMAND ${CMAKE_COMMAND} -E env SECOND_MAP=0,0,1,1,2,2
              sh ${CMAKE_CURRENT_SOURCE_DIR}/restart-leftovers.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_stalled-start> $<TARGET_FILE:shmem_test_internode>
              $<TARGET_FILE:slow-create>)
    set_tests_properties(shmem.leftovers.restart.other-size PROPERTIES LABELS "tier2"
      TIMEOUT 120 PASS_REGULAR_EXPRESSION "RESTART LEFTOVERS OK")
    osoe_internode_label(shmem.leftovers.restart.slow-lead shmem.leftovers.restart.other-size)
  endif()

  # The launcher's signals, on two nodes (above: on one).
  foreach(mode term int kill)
    add_test(NAME shmem.launcher-signals.${mode}
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/launcher-signals.sh
              $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_sleeper> ${mode})
    set_tests_properties(shmem.launcher-signals.${mode} PROPERTIES LABELS "tier2"
      TIMEOUT 60 PASS_REGULAR_EXPRESSION "${osoe_launcher_signals_${mode}}")
  endforeach()
  osoe_internode_label(shmem.launcher-signals.term shmem.launcher-signals.int
    shmem.launcher-signals.kill)
endif()

# What a core holds: this PE's heap, not its siblings' or the connection area
# (review round two, omissions 4).
osoe_tier2_map_test(core-exclusion 0,0,1,1)
osoe_tier2_map_test(core-exclusion 0,0)

# A request larger than /dev/shm is refused at init, by name (§6.2; review round
# two, omissions 5). See dev-shm-capacity.sh.
add_test(NAME shmem.dev-shm-capacity.heaps
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> heaps)
# internode: dev-shm-capacity.sh's connections mode launches a job of several nodes
if(OSOE_INTERNODE)
  add_test(NAME shmem.dev-shm-capacity.connections
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
            $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> connections)
endif()
# And OSOE_SHM_CAPACITY_CHECK=0, for testing on a small /dev/shm: the same
# request is reported and the job runs.
add_test(NAME shmem.dev-shm-capacity.warned
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> warned)
# And the node's PEs end with it. The lead checks the room once its control
# segment exists, so that the refusal has the segment's `failed` word to go
# through: checked before, its node-mates waited for a segment that never came
# (the round-three review's R8). Run with -k, so that the launcher ends none of
# them for it.
add_test(NAME shmem.dev-shm-capacity.node-ends
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> node-ends)
set_tests_properties(shmem.dev-shm-capacity.node-ends PROPERTIES
  LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "REFUSAL OK")
# And with the launcher's output in a pipe whose reader has gone: the launcher
# still removes what the refused start left, and exits 1, not 141 (the v0.2
# step 1 review).
add_test(NAME shmem.dev-shm-capacity.broken-pipe
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dev-shm-capacity.sh
          $<TARGET_FILE:osoe-launch> $<TARGET_FILE:shmem_test_init> broken-pipe)
set_tests_properties(shmem.dev-shm-capacity.broken-pipe PROPERTIES
  LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "BROKEN PIPE OK")
set_tests_properties(shmem.dev-shm-capacity.heaps shmem.dev-shm-capacity.warned
  PROPERTIES LABELS "tier2" TIMEOUT 60)
set_tests_properties(shmem.dev-shm-capacity.warned PROPERTIES PASS_REGULAR_EXPRESSION
  "and 2 heaps.*continuing, since OSOE_SHM_CAPACITY_CHECK=0.*launcher exited 0")
set_tests_properties(shmem.dev-shm-capacity.heaps PROPERTIES PASS_REGULAR_EXPRESSION
  "/dev/shm has [0-9]+ MiB free, but this node's control segment and 2 heaps.*launcher exited 1")
if(OSOE_INTERNODE)
  set_tests_properties(shmem.dev-shm-capacity.connections PROPERTIES LABELS "tier2" TIMEOUT 60)
  set_tests_properties(shmem.dev-shm-capacity.connections PROPERTIES PASS_REGULAR_EXPRESSION
    "/dev/shm has [0-9]+ MiB free, but this node's connection area.*launcher exited 1"
    SKIP_RETURN_CODE 77)
  osoe_internode_label(shmem.dev-shm-capacity.connections)
endif()

if(OSOE_INTERNODE)
  # The notice that the connection buffers were shrunk to fit the budget is one
  # line a node, from its lead, not one a PE (the round-three review's P3: 128,000
  # lines at 500 nodes of 256 PEs): three nodes of three PEs, and a buffer the
  # budget cannot hold. See shrink-notice.sh.
  add_test(NAME shmem.am-size.notice
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/shrink-notice.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_barrier>)
  set_tests_properties(shmem.am-size.notice PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "SHRINK NOTICE OK")
  # internode: shrink-notice.sh launches a job of three nodes
  osoe_internode_label(shmem.am-size.notice)

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
  # A sandbox that lets the private futex2 calls through and refuses the shared
  # ones, which every wait between a node's PEs makes: refused by name at
  # shmem_init too, since the check tries both scopes - before the adversarial
  # read of v0.2 step 1's parts 2 and 3 it tried the private one alone, and a job
  # died of SIGABRT at its first shared wait, its objects already made.
  add_test(NAME shmem.refuse.no-shared-futex2
    COMMAND deny-futex2 --shared $<TARGET_FILE:osoe-launch> -n 2 -t 30 --
            $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.refuse.no-shared-futex2 PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION
      "needs Linux 6.7 or later, for the futex2 calls \\(they fail with \"Operation not permitted\"\\)"
    FAIL_REGULAR_EXPRESSION "left behind|futex_wake failed|futex_wait failed")
  # Labelled internode because the single-node build has no futex2 to deny (it waits
  # with the classic futex), so the refusals these three check cannot happen there.
  osoe_internode_label(shmem.refuse.old-kernel env.probe.old-kernel
                       shmem.refuse.no-shared-futex2)

endif()

# io_uring refused in each place shmem_init's check enters it (deny-io-uring.c):
# the probe fails its check of shmem_init's, naming the place; a job of two
# nodes is refused by name before anything is made, the sysctl named only where
# it can be the cause; and a job of one node, which opens no ring, runs - in
# both builds, the single-node build having none to open. Before Part 6's
# adversarial reads the check missed `enable` and `enter`, and a job ended after
# its PEs had left shmem_init.
add_executable(deny-io-uring deny-io-uring.c)
osoe_warnings(deny-io-uring)
add_test(NAME shmem.no-io-uring.one-node
  COMMAND deny-io-uring setup $<TARGET_FILE:osoe-launch> -n 2 -t 30 --
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.no-io-uring.one-node PROPERTIES LABELS "tier2" TIMEOUT 60)
if(OSOE_INTERNODE)
  set(osoe_io_uring_refused_setup "an io_uring created SINGLE_ISSUER, R_DISABLED and CQSIZE")
  set(osoe_io_uring_refused_enable "an io_uring enabled after it was created R_DISABLED")
  set(osoe_io_uring_refused_enter "an io_uring that can be entered to wait for completions")
  foreach(where setup enable enter)
    add_test(NAME env.probe.no-io-uring-${where}
      COMMAND deny-io-uring ${where} $<TARGET_FILE:probe>)
    set_tests_properties(env.probe.no-io-uring-${where} PROPERTIES LABELS "env" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "FAIL shmem_init's io_uring check +${osoe_io_uring_refused_${where}}")
    add_test(NAME shmem.refuse.no-io-uring-${where}
      COMMAND deny-io-uring ${where} $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 30 --
              $<TARGET_FILE:shmem_test_init>)
    if(where STREQUAL "setup")
      set(osoe_hint " .*which usually means the kernel.io_uring_disabled sysctl is set")
      set(osoe_not "left behind")
    else()
      set(osoe_hint "")
      set(osoe_not "left behind|io_uring_disabled|submission failed|cannot enable")
    endif()
    set_tests_properties(shmem.refuse.no-io-uring-${where} PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION
        "osoe needs io_uring with ${osoe_io_uring_refused_${where}} for a job of 2 nodes${osoe_hint}"
      FAIL_REGULAR_EXPRESSION "${osoe_not}")
    # internode: the single-node build has no io_uring to deny
    osoe_internode_label(env.probe.no-io-uring-${where} shmem.refuse.no-io-uring-${where})
  endforeach()
endif()

# The single-node build's own tests (v0.2 step 1), registered there alone: a
# job of more than one node refused at shmem_init by name - every PE alike,
# each ending by itself with a failure status, nothing left in /dev/shm, which
# the launcher would report - and the classic futex refused by a sandbox,
# refused by name as the full build refuses a kernel without futex2 (above),
# by shmem_init and by the probe.
if(OSOE_SINGLE_NODE)
  osoe_refusal_test(shmem.single-node.refuse-nodes 2 0
    LAUNCH -n 2 -m 0,1
    PATTERNS "this is osoe's single-node build \\(OSOE_SINGLE_NODE\\), which runs a job of one node, and this job spans 2 nodes: the full build runs it"
             "!left behind"
             "!control segment"
    PROGRAM $<TARGET_FILE:shmem_test_init>)
  # The version names the build, written here as a literal: every version test
  # compares a tool with OSOE_VERSION_TEXT, which the suffix is part of, so a
  # build that dropped it would pass them all (the v0.2 step 1 review, Q5-5).
  add_test(NAME shmem.single-node.version-says-so COMMAND $<TARGET_FILE:osoe-launch> --version)
  set_tests_properties(shmem.single-node.version-says-so PROPERTIES LABELS "tier1" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "osoe-launch [(]osoe[)] [0-9.]+ [(]single-node build[)]")
  add_executable(deny-futex deny-futex.c)
  osoe_warnings(deny-futex)
  # Not under AddressSanitizer: GCC 14's runtime takes futex locks of its own as
  # a process starts, and glibc ends a process whose futex call fails as it
  # never should ("The futex facility returned an unexpected error code"), so
  # under the filter every program ends before shmem_init's check could run.
  # GCC 11's runtime and ThreadSanitizer's run them.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=address")
    add_test(NAME shmem.refuse.no-futex
      COMMAND deny-futex $<TARGET_FILE:osoe-launch> -n 2 -t 30 --
              $<TARGET_FILE:shmem_test_init>)
    set_tests_properties(shmem.refuse.no-futex PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION
        "needs the futex calls FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, which a sandbox \\(seccomp\\) must allow .*this kernel is"
      FAIL_REGULAR_EXPRESSION "left behind|futex wake failed|futex wait failed")
    add_test(NAME env.probe.no-futex COMMAND deny-futex $<TARGET_FILE:probe>)
    set_tests_properties(env.probe.no-futex PROPERTIES LABELS "env" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "FAIL futex U32 wait \\(value mismatch\\)")
    # A sandbox that allows the private calls alone, which glibc's own locks
    # make, and refuses the shared ones every wait between a node's PEs makes:
    # refused by name at shmem_init too, nothing made, since the check tries
    # both scopes (shmem.refuse.no-shared-futex2 is the full build's).
    add_test(NAME shmem.refuse.no-shared-futex
      COMMAND deny-futex --shared $<TARGET_FILE:osoe-launch> -n 2 -t 30 --
              $<TARGET_FILE:shmem_test_init>)
    set_tests_properties(shmem.refuse.no-shared-futex PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION
        "needs the futex calls FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, which a sandbox \\(seccomp\\) must allow \\(they fail with \"Operation not permitted\"\\)"
      FAIL_REGULAR_EXPRESSION "left behind|futex wake failed|futex wait failed")
  endif()
endif()

# The heap holds what SHMEM_SYMMETRIC_SIZE says, its specification's own example
# among the values (§8; the v0.2 step 1 review, C1): 3.1M is 3,250,586 bytes,
# not a multiple of 16, and 1000 is not either.
add_executable(shmem_test_heap-size test-heap-size.c)
target_link_libraries(shmem_test_heap-size PRIVATE osoe)
osoe_warnings(shmem_test_heap-size)
foreach(case "3.1M|3250586" "1000|1000")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 size)
  list(GET parts 1 bytes)
  add_test(NAME shmem.heap-size.${size}
    COMMAND ${CMAKE_COMMAND} -E env SHMEM_SYMMETRIC_SIZE=${size} OSOE_TEST_HEAP_BYTES=${bytes}
            $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_heap-size>)
  set_tests_properties(shmem.heap-size.${size} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "holds ${bytes} bytes: yes")
endforeach()

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

# shmem.amo-deprecated over a logical node map (§8.1), so "right" is a
# same-node PE on one member of the pair and a remote one on the other, as
# shmem.typed-rma's maps are chosen for below. Excluded under ThreadSanitizer
# for the same reason: the ordering that makes a cross-node AMO's completion
# visible runs through another process, which tsan cannot see.
osoe_tier2_test(amo-deprecated)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(amo-deprecated 0,0,1,1)
  osoe_tier2_map_test(amo-deprecated 0,1,2,0,1,0)
endif()

# shmem.amo-nbi (§9.7.2; test-amo-nbi.c), over a logical node map (§8.1): its
# semantics phase's second pass targets each PE's right neighbour, and its
# contended phase has every PE issue fetch_inc_nbi/fetch_add_nbi at PE 0, each
# same-node for some PEs and remote for others depending on the map, as
# shmem.amo-deprecated's right-neighbour targets are just above. Excluded
# under ThreadSanitizer for the same reason: the ordering that makes a
# cross-node AMO's completion visible runs through another process, which
# tsan cannot see. And a nonblocking AMO on a word outside the symmetric heap
# must end the job under its own name, not the blocking routine's beneath it.
osoe_tier2_test(amo-nbi)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(amo-nbi 0,0,1,1)
  osoe_tier2_map_test(amo-nbi 0,1,2,0,1,0)
endif()
add_test(NAME shmem.amo-nbi.misuse
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_AMO_NBI_MISUSE=1
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_amo-nbi>)
set_tests_properties(shmem.amo-nbi.misuse PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_long_atomic_fetch_add_nbi: \\[0x[0-9a-f]+, \\+8\\) is n")

osoe_tier2_test(typed-rma)

# shmem.typed-rma over a logical node map (§8.1), so "right" is a same-node PE
# on one member of the pair and a remote one on the other - the program itself
# never names the difference (test-typed-rma.c). Two maps, as the neighbouring
# families use for the same reason: a 2-node pair and the 3-node, 6-PE map
# shmem.internode.* also runs. Excluded under ThreadSanitizer for exactly the
# reason shmem.internode.* is (above): the ordering that makes a cross-node put
# safe runs through another process, which tsan cannot see.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(typed-rma 0,0,1,1)
  osoe_tier2_map_test(typed-rma 0,1,2,0,1,0)
endif()
if(OSOE_INTERNODE)
  # Under §8.1's force-remote flag: a same-node PE is denied the intranode
  # pointer too, so every block of an ibput/ibget's loop (as every element of an
  # iput/iget's) takes the AM path regardless of the map, as the collectives'
  # own force-remote test does. Multi-node maps only, and left out under tsan for
  # shmem.internode.*'s reason (above).
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.typed-rma.force-remote.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
              $<TARGET_FILE:shmem_test_typed-rma>)
    set_tests_properties(shmem.typed-rma.force-remote.0-0-1-1 PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_internode_label(shmem.typed-rma.force-remote.0-0-1-1)
  endif()
endif()
# shmem_ibput's dst/sst below bsize is undefined (spec §9.6.1.4's Arguments)
# and ends the job by name (strided_block_put, rma-typed.c), as the other
# refusals are (shmem.lock.misuse.*, shmem.statics.misuse).
add_test(NAME shmem.typed-rma.ibput-misuse
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_IBPUT_MISUSE=1
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_typed-rma>)
set_tests_properties(shmem.typed-rma.ibput-misuse PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_long_ibput: dst 1, sst 2 below bsize 2; spec")

# Nonblocking gets (§4.6, as built; test-get-nbi.c): on one node, where every
# get is applied before it returns; and across nodes with the landing records
# and arena small, so that an issue finds them used up and completes what is in
# flight first, and with two reply credits, so that the lead's return of each
# credit as its reply lands is what lets the next get out. tsan is left out of
# the maps for shmem.internode.*'s reason: the lead writes the reply from
# another process.
osoe_tier2_test(get-nbi)

# The reductions and broadcast (§5.5, phase 1 step 5; test-reduce.c,
# test-broadcast.c): on one node at 1, 2 and 4 PEs, where only the intranode
# accumulator runs; across nodes on a block and an interleaved map; and on five
# nodes at fan-out 2, where the leads' tree has an interior node that both
# combines its children's pieces and passes the result on. tsan is left out of
# the maps for shmem.internode.*'s reason.
foreach(collective reduce broadcast)
  osoe_tier2_test(${collective})
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    osoe_tier2_map_test(${collective} 0,0,1,1)
    osoe_tier2_map_test(${collective} 0,1,2,0,1,0)
    osoe_tier2_map_test(${collective} 0,1,2,3,4 OSOE_COLLECTIVE_FANOUT=2)
  endif()
endforeach()
if(OSOE_INTERNODE)
  # Under OSOE_FORCE_REMOTE, SHMEM_TEAM_SHARED collapses to the PE itself: the
  # data collectives' team of one (collectives.c), which no other map reaches.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    foreach(collective reduce broadcast)
      add_test(NAME shmem.${collective}.force-remote.0-0-1-1
        COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1
                $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
                $<TARGET_FILE:shmem_test_${collective}>)
      set_tests_properties(shmem.${collective}.force-remote.0-0-1-1 PROPERTIES
        LABELS "tier2" TIMEOUT 90)
      osoe_internode_label(shmem.${collective}.force-remote.0-0-1-1)
    endforeach()
  endif()
endif()

# The public headers compiled under each standard they promise, every warning
# an error (headers.c, headers.cpp): no other translation unit is C99 or C++,
# so without these a break in either mode would ship. C++ where the machine has
# a compiler for it.
foreach(standard c99 c11 c17)
  add_test(NAME shmem.headers.${standard}
    COMMAND ${CMAKE_C_COMPILER} -std=${standard} -Wall -Wextra -Wpedantic -Werror
            -fsyntax-only -I${PROJECT_SOURCE_DIR}/shmem/include
            ${CMAKE_CURRENT_SOURCE_DIR}/headers.c)
  set_tests_properties(shmem.headers.${standard} PROPERTIES LABELS "tier1" TIMEOUT 30)
endforeach()
find_program(OSOE_CXX_COMPILER NAMES c++ g++ clang++)
if(OSOE_CXX_COMPILER)
  foreach(standard c++11 c++17)
    add_test(NAME shmem.headers.${standard}
      COMMAND ${OSOE_CXX_COMPILER} -std=${standard} -Wall -Wextra -Wpedantic -Werror
              -fsyntax-only -I${PROJECT_SOURCE_DIR}/shmem/include
              ${CMAKE_CURRENT_SOURCE_DIR}/headers.cpp)
    set_tests_properties(shmem.headers.${standard} PROPERTIES LABELS "tier1" TIMEOUT 30)
  endforeach()
else()
  message(STATUS "No C++ compiler: the headers' C++ test is not registered")
endif()

# A C11 generic called with the wrong number of arguments fails to compile,
# naming what it takes (generic-arity.c): one wrong call from each family the
# selection serves.
foreach(pair "fetch_add_nbi:osoe_shmem_atomic_fetch_add_nbi_takes_four_or_five_arguments"
             "fetch_add:osoe_shmem_atomic_fetch_add_takes_three_or_four_arguments"
             "put:osoe_shmem_put_takes_four_or_five_arguments"
             "signal_add:osoe_shmem_signal_add_takes_three_or_four_arguments"
             "signal_set:osoe_shmem_signal_set_takes_three_or_four_arguments")
  string(REPLACE ":" ";" pair "${pair}")
  list(GET pair 0 call)
  list(GET pair 1 complaint)
  add_test(NAME shmem.generic-arity.${call}
    COMMAND ${CMAKE_C_COMPILER} -std=c11 -Werror=implicit-function-declaration
            -fsyntax-only -DOSOE_TEST_ARITY_${call} -I${PROJECT_SOURCE_DIR}/shmem/include
            ${CMAKE_CURRENT_SOURCE_DIR}/generic-arity.c)
  set_tests_properties(shmem.generic-arity.${call} PROPERTIES LABELS "tier1" TIMEOUT 30
    PASS_REGULAR_EXPRESSION "${complaint}")
endforeach()

# The deprecated active-set collectives over the whole job (design §7.4;
# test-active-set.c), on one node and across nodes; and an active set reaching
# past the job, which must end it by name.
osoe_tier2_test(active-set)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(active-set 0,0,1,1)
  osoe_tier2_map_test(active-set 0,1,2,0,1,0)
endif()
add_test(NAME shmem.active-set.partial
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_PARTIAL=1
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_active-set>)
set_tests_properties(shmem.active-set.partial PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION
  "shmem_long_sum_to_all: the active set \\(PE_start 0, logPE_stride 0, PE_size 3\\) is not a set of this job's 2 PEs")

# The deprecated collectives on active sets within the job but not all of it
# (phase 1 step 5 slice 7; active-set.c; test-active-set-partial.c): six sets,
# two of them at once, every routine, pSync reused, on one node and across
# nodes on a block and an interleaved map.
osoe_tier2_test(active-set-partial)
# Across nodes not under tsan, like its whole-job sibling: a remote write's
# order runs through other processes, which tsan cannot see.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(active-set-partial 0,0,1,1)
  osoe_tier2_map_test(active-set-partial 0,1,2,0,1,0)
  # Twelve PEs: sets of 11, with interior members of two and three children
  # and trees four deep, and a stride of four.
  osoe_tier2_map_test(active-set-partial 0,0,0,1,1,1,2,2,2,3,3,3)
endif()
add_test(NAME shmem.active-set-partial.bad-root
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_BAD_ROOT=1
          $<TARGET_FILE:osoe-launch> -n 3 -t 30 -- $<TARGET_FILE:shmem_test_active-set-partial>)
set_tests_properties(shmem.active-set-partial.bad-root PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_broadcast64: PE_root 2 is not a rank of its active set of 2")

# Symmetric statics (design §4.3; test-statics.c), in a program linked as oshcc
# links one: on one node, across nodes on a block map and an interleaved one;
# and a put to a stack address, which must name both regions it is not in.
osoe_tier2_test(statics)
osoe_statics_program(shmem_test_statics)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(statics 0,0,1,1)
  osoe_tier2_map_test(statics 0,1,2,0,1,0)
endif()
# The same test built -fpie and -pie with the script: every PE's statics at an
# address of its own, which symmetric operations must not care about.
add_executable(shmem_test_statics-pie test-statics.c ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_statics-pie PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_statics-pie PRIVATE osoe rt)
target_compile_definitions(shmem_test_statics-pie PRIVATE OSOE_TEST_STATICS_PIE)
target_compile_options(shmem_test_statics-pie PRIVATE -fpie)
target_link_options(shmem_test_statics-pie PRIVATE -pie
  "LINKER:-T,${PROJECT_SOURCE_DIR}/tools/wrapper/osoe-statics.ld")
osoe_warnings(shmem_test_statics-pie)
add_test(NAME shmem.statics.pie.L2
  COMMAND osoe-launch -n 2 -t 60 -- $<TARGET_FILE:shmem_test_statics-pie>)
set_tests_properties(shmem.statics.pie.L2 PROPERTIES LABELS "tier2" TIMEOUT 90)
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.statics.pie.0-0-1-1
      COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 60 -- $<TARGET_FILE:shmem_test_statics-pie>)
    set_tests_properties(shmem.statics.pie.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 90)
    osoe_internode_label(shmem.statics.pie.0-0-1-1)
  endif()
endif()
add_test(NAME shmem.statics.misuse
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STATICS_MISUSE=stack
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_statics>)
set_tests_properties(shmem.statics.misuse PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION
  "shmem_int_p: .* is neither within the symmetric heap nor among the program's global and static variables")

# The distributed lock (spec §9.13; test-lock.c): one node and across nodes;
# and a clear of a lock not held, and a set of one held, each ending the job by
# name.
osoe_tier2_test(lock)
osoe_statics_program(shmem_test_lock)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(lock 0,0,1,1)
  osoe_tier2_map_test(lock 0,1,2,0,1,0)
endif()
foreach(case_expect
    "clear|shmem_clear_lock: this PE does not hold the lock it clears"
    "twice|shmem_set_lock: this PE holds the lock already"
    "garbage|shmem_set_lock: the lock's queue names PE [0-9]+, which is not waiting for it")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 case)
  list(GET parts 1 expect)
  add_test(NAME shmem.lock.misuse.${case}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_LOCK_MISUSE=${case}
            $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_lock>)
  set_tests_properties(shmem.lock.misuse.${case} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "${expect}")
endforeach()

# collect and fcollect, alltoall and alltoalls (spec §9.10.5, §9.10.6,
# §9.10.8; test-collect.c, test-alltoall.c): on one node, and across nodes on a
# block map and an interleaved one; and an fcollect64 over an active set one PE
# short of the job, which must end it by name.
foreach(name collect alltoall)
  osoe_tier2_test(${name})
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    osoe_tier2_map_test(${name} 0,0,1,1)
    osoe_tier2_map_test(${name} 0,1,2,0,1,0)
  endif()
endforeach()
# The collect test's pSync is a static, as the corpus declares it (§4.3); and
# over four nodes at a fan-out of 2, so a gather passes through a lead that is
# neither the root nor a leaf.
osoe_statics_program(shmem_test_collect)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(collect 0,1,2,3 OSOE_COLLECTIVE_FANOUT=2)
  # And twenty PEs on five nodes with OSOE_SMALL_COLLECTIVE_BYTES at its
  # smallest, so that collect's lengths - eight bytes a member, 160 here - are
  # a large reduction (the team redesign's step E), entered before the lead
  # waits for its members, its children waiting for the lead's grant.
  osoe_tier2_map_test(collect 0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4
                      OSOE_SMALL_COLLECTIVE_BYTES=128)
endif()
add_test(NAME shmem.alltoall.stride
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_ALLTOALL_STRIDE=0
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_alltoall>)
set_tests_properties(shmem.alltoall.stride PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "shmem_long_alltoalls: strides 0 and 1; both must be at least 1")
add_test(NAME shmem.collect.partial
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_PARTIAL=1
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_collect>)
set_tests_properties(shmem.collect.partial PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION
  "shmem_fcollect64: PE 1 called it, and is not in its active set \\(PE_start 0, logPE_stride 0, PE_size 1\\)")

if(OSOE_INTERNODE)
  # PEs that call different collectives end the job by name, from the piece
  # headers of the generic internode phase (internode-generic.c): node 1's PE
  # broadcasts where the others reduce, and node 0's lead finds node 1's slot
  # holding no partial result. Three nodes, so that node 0 also has a child that
  # did call the reduction.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_executable(shmem_test_collective-divergence test-collective-divergence.c)
    target_link_libraries(shmem_test_collective-divergence PRIVATE osoe)
    osoe_warnings(shmem_test_collective-divergence)
    add_test(NAME shmem.collective-divergence
      COMMAND osoe-launch -n 3 -m 0,1,2 -t 30 -e 1:OSOE_TEST_DIVERGE=1 --
              $<TARGET_FILE:shmem_test_collective-divergence>)
    set_tests_properties(shmem.collective-divergence PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "PEs called different collectives")
    # A gather's size differing between two nodes: the one PE fcollects two longs
    # where the other fcollects one, and a lead finds the shape the other said it
    # entered is not its own before a byte lands (phase 1 step 5 slice 6).
    add_test(NAME shmem.collective-divergence.gather-size
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_DIVERGE_KIND=gather
              $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 30 -e 1:OSOE_TEST_DIVERGE=1 --
              $<TARGET_FILE:shmem_test_collective-divergence>)
    set_tests_properties(shmem.collective-divergence.gather-size PROPERTIES LABELS "tier2"
      TIMEOUT 60 PASS_REGULAR_EXPRESSION "PEs called different collectives")
    osoe_internode_label(shmem.collective-divergence shmem.collective-divergence.gather-size)
  endif()

  # Pipelined pieces through the internode tree (design §5.8; phase 1 step 5
  # slice 5; test-pipeline.c): a reduction, a broadcast from a root off PE 0's
  # node and an fcollect, each of 1 MiB and of an odd size, every element
  # checked; collectives back to back from every root, their trees rotating; and
  # the count each lead publishes of the most pieces it had in flight, by its
  # place in the tree. On a block map, an interleaved one, five nodes at a
  # fan-out of 2, where a lead is both a parent and a child, and eight, a tree
  # three deep, at D = 1, 2 and 4 - D = 1 being one piece at a time. tsan is left
  # out of the maps for shmem.internode.*'s reason. Under OSOE_COLLECTIVE_SHAPE=tree
  # since v0.2 step 3, so that a 1 MiB reduction keeps to the tree whose counts
  # these pin, where the link might choose Träff's (shmem.circulant.* below).
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_executable(shmem_test_pipeline test-pipeline.c ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
    target_include_directories(shmem_test_pipeline PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
    target_link_libraries(shmem_test_pipeline PRIVATE osoe rt)
    osoe_warnings(shmem_test_pipeline)
    foreach(case "0,0,1,1" "0,1,2,0,1,0" "0,1,2,3,4:OSOE_COLLECTIVE_FANOUT=2"
                 "0,1,2,3,4,5,6,7:OSOE_COLLECTIVE_FANOUT=2")
      string(REPLACE ":" ";" environment "${case}")
      list(POP_FRONT environment map)
      string(REPLACE "," ";" ranks "${map}")
      list(LENGTH ranks count)
      string(REPLACE "," "-" label "${map}")
      foreach(in_flight 1 2 4)
        add_test(NAME shmem.pipeline.${label}.in-flight-${in_flight}
          COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree
                  OSOE_COLLECTIVE_PIPELINE=${in_flight} ${environment}
                  $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
                  $<TARGET_FILE:shmem_test_pipeline>)
        set_tests_properties(shmem.pipeline.${label}.in-flight-${in_flight} PROPERTIES
          LABELS "tier2" TIMEOUT 90)
        osoe_internode_label(shmem.pipeline.${label}.in-flight-${in_flight})
      endforeach()
    endforeach()
    # Under the AM stress settings - 1 KiB chunks, two credits, the wire in short
    # reads split at random - where a piece is sixteen chunks and a lead's puts
    # queue behind its credits. And with OSOE_MAX_TEAMS=4 and OSOE_MAX_THREADS=1,
    # the fewest team numbers and lanes - the main thread's and the agent's - so
    # that the region is as small as it gets and its lanes end it: a slot
    # computed from piece k rather than k mod D, which a larger region lets write
    # into a lane nobody lent unseen, runs past the region's end, which the
    # target refuses by name (progress-host.c). At D = 2.
    add_test(NAME shmem.pipeline.0-1-2-3-4.stress
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree OSOE_COLLECTIVE_PIPELINE=2
              OSOE_COLLECTIVE_FANOUT=2
              OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2
              OSOE_WIRE_BYTES=256 OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20260919
              $<TARGET_FILE:osoe-launch> -n 5 -m 0,1,2,3,4 -t 150 --
              $<TARGET_FILE:shmem_test_pipeline>)
    set_tests_properties(shmem.pipeline.0-1-2-3-4.stress PROPERTIES LABELS "tier2" TIMEOUT 180)
    add_test(NAME shmem.pipeline.0-0-1-1.max-teams-4
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree OSOE_COLLECTIVE_PIPELINE=2
              OSOE_MAX_TEAMS=4
              OSOE_MAX_THREADS=1
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
              $<TARGET_FILE:shmem_test_pipeline>)
    set_tests_properties(shmem.pipeline.0-0-1-1.max-teams-4 PROPERTIES LABELS "tier2" TIMEOUT 90)
    osoe_internode_label(shmem.pipeline.0-1-2-3-4.stress shmem.pipeline.0-0-1-1.max-teams-4)
  endif()

  # Lanes and grants (design §5.8; the team redesign's step E; test-lanes.c): the
  # cases of tools/modelcheck/lane_model.py on the code, one PE a logical node.
  # Two teams whose leads coincide on node 1, the parent of both, with a child
  # outside the collective its parent is in (shared-lead), and under
  # SHMEM_THREAD_MULTIPLE their large reductions at once on two of node 1's
  # threads (two-threads); a team's large reductions led by its PEs' threads in
  # turn (turns); a broadcast's tree turned against the reduction's before it,
  # under OSOE_TEST_PERTURB (rotation); the class boundary a byte either side, at
  # the setting's smallest and at its default, and a large collective's pieces in
  # flight at D = 2 and 4 (boundary); a team number taken again (reuse); and two
  # leads in different reductions, which the grant names (mismatch). They run
  # under tsan too, which shmem.internode.*'s maps are kept from: that reason
  # needs a node-mate, a PE whose heap the node's lead's agent writes from
  # another process, and every case here is one PE a logical node. The agent that
  # applies a PE's incoming puts is a thread of that PE's own process, and what
  # separates its write into a lane from the owner's last touch of it - the
  # owner's own send through the agent, and the counters - is all inside the one
  # process tsan sees (round six's H5 ran the nine positive cases under it clean,
  # and a lane shared by threads is a data race it reports). All but
  # class-split-hang, which ends by the deadlock detector: under tsan no job is
  # judged deadlocked, its own thread in every process being one the detector
  # cannot see (shmem.deadlock.* below). Under OSOE_COLLECTIVE_SHAPE=tree since
  # v0.2 step 3: these are the trees' lanes and grants, whose counts and messages
  # they pin, and a measured link can take a large reduction on two to four
  # nodes into Träff's, whose lanes shmem.circulant.* tests (reuse and turns
  # among them).
  add_executable(shmem_test_lanes test-lanes.c ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_lanes PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_lanes PRIVATE osoe rt Threads::Threads)
  osoe_warnings(shmem_test_lanes)
  foreach(case "shared-lead:0,1,2,3" "two-threads:0,1,2,3" "turns:0,1"
               "rotation:0,1,2,3:OSOE_COLLECTIVE_FANOUT=2:OSOE_TEST_PERTURB=1"
               "boundary:0,1,2" "boundary:0,1,2:OSOE_SMALL_COLLECTIVE_BYTES=128"
               "boundary:0,1,2:OSOE_COLLECTIVE_PIPELINE=4" "reuse:0,1"
               "scan-shared-parent:0,1,2,3" "scan-turns:0,1" "scan-reuse:0,1")
    string(REPLACE ":" ";" environment "${case}")
    list(POP_FRONT environment which map)
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    set(label "${which}")
    foreach(setting IN LISTS environment)
      string(REGEX REPLACE "^OSOE_([A-Z_]+)=(.*)$" "\\1-\\2" suffix "${setting}")
      string(TOLOWER "${suffix}" suffix)
      string(REPLACE "_" "-" suffix "${suffix}")
      if(NOT setting MATCHES "PERTURB|FANOUT")
        set(label "${label}.${suffix}")
      endif()
    endforeach()
    add_test(NAME shmem.lanes.${label}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree ${environment}
              $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 90 --
              $<TARGET_FILE:shmem_test_lanes> ${which})
    set_tests_properties(shmem.lanes.${label} PROPERTIES LABELS "tier2" TIMEOUT 120)
    osoe_internode_label(shmem.lanes.${label})
  endforeach()
  add_test(NAME shmem.lanes.mismatch
    COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_lanes>
            mismatch)
  set_tests_properties(shmem.lanes.mismatch PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION
    "the grant from node 0's lead [(]PE 0[)] is for epoch [0-9]+'s sum of 8-byte signed integers, 4096 bytes, and this node's lead [(]PE 1[)] is in epoch [0-9]+'s maximum of 8-byte signed integers")
  add_test(NAME shmem.lanes.scan-mismatch
    COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_lanes>
            scan-mismatch)
  set_tests_properties(shmem.lanes.scan-mismatch PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION
    "the scan grant from PE 0 is for scan [0-9]+ of the team, an inclusive one of 8-byte signed integers, 4096 bytes, and PE 1 is in scan [0-9]+ of the team, an exclusive one of 8-byte signed integers")
  add_test(NAME shmem.lanes.class-split-named
    COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_lanes>
            class-split-named)
  set_tests_properties(shmem.lanes.class-split-named PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION
    "the lane word of node 1's lead [(]PE 1[)], this node's child at position 0, says epoch 0 and lane 0, and this node's lead [(]PE 0[)] is in epoch [0-9]+'s collective")
  add_test(NAME shmem.lanes.class-split-broadcast
    COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=tree
            $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_lanes>
            class-split-broadcast)
  set_tests_properties(shmem.lanes.class-split-broadcast PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION
    "should be piece 0 [(]1500 bytes[)] of epoch [0-9]+'s broadcast from the node at index 0, and is nothing: no piece was put there")
  osoe_internode_label(shmem.lanes.mismatch shmem.lanes.scan-mismatch
    shmem.lanes.class-split-named shmem.lanes.class-split-broadcast)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.lanes.class-split-hang
      COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=1 OSOE_COLLECTIVE_SHAPE=tree
              $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_lanes>
              class-split-hang)
    set_tests_properties(shmem.lanes.class-split-hang PROPERTIES LABELS "tier2" TIMEOUT 90
      PASS_REGULAR_EXPRESSION "every PE of the job waits in the library.*: PE [0-9, ]+ in shmem_long_sum_reduce"
      FAIL_REGULAR_EXPRESSION "osoe-launch: timeout")
    osoe_internode_label(shmem.lanes.class-split-hang)
  endif()

  # A lane for each thread, for every kind of collective (test-lanes-threads.c;
  # round six's H1, the correctness investigator's stress): four threads a PE,
  # each with its own copy of the world split every round into a team of its own
  # shape, then sums, broadcasts and both scans of 8 to 5,000 longs on either side
  # of the class line, 300 rounds, every result checked. What the cases above do
  # not see: a regression that sends every large scan of a PE through one lane
  # returns wrong prefix sums with status 0, and passes shmem.lanes.* and
  # shmem.scan.* whole. On one PE a logical node, which tsan sees whole; and on two
  # a node, which it does not: a lead's agent writes its node-mate's heap from
  # another process (shmem.internode.*'s reason, above), so that map is not run
  # under it.
  add_executable(shmem_test_lanes_threads test-lanes-threads.c)
  target_link_libraries(shmem_test_lanes_threads PRIVATE osoe Threads::Threads)
  osoe_warnings(shmem_test_lanes_threads)
  add_test(NAME shmem.lanes.threads
    COMMAND $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 120 --
            $<TARGET_FILE:shmem_test_lanes_threads> 300 4)
  set_tests_properties(shmem.lanes.threads PROPERTIES LABELS "tier2" TIMEOUT 150)
  osoe_internode_label(shmem.lanes.threads)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.lanes.threads.two-a-node
      COMMAND $<TARGET_FILE:osoe-launch> -n 6 -m 0,0,1,1,2,2 -t 120 --
              $<TARGET_FILE:shmem_test_lanes_threads> 300 4)
    set_tests_properties(shmem.lanes.threads.two-a-node PROPERTIES LABELS "tier2" TIMEOUT 150)
    osoe_internode_label(shmem.lanes.threads.two-a-node)
  endif()

  # The allgather among node leads that is fcollect's and collect's internode
  # phase (design §5.8, "An allgather for fcollect"; phase 1 step 5 slice 6;
  # test-allgather.c): blocks of 1 byte, 8 bytes and 64 KiB and a collect's
  # differing ones, into the heap and into both statics ranges, byte by byte;
  # fcollects back to back into one dest with the team's rank 0 reading each
  # result late; and the steps and shares each lead publishes. On the world, its
  # even ranks and the world reversed, over two to five logical nodes - so that
  # the node count is and is not a power of two - on a block map and a cyclic
  # one of two PEs a node, and on an interleaved map of uneven nodes; and on
  # five, a team spanning three nodes: on the block map ranks 3 to 7, nodes 1 to
  # 3 unevenly, and on the cyclic one ranks 2, 5 and 8, nodes 2, 0 and 3 in that
  # order of team rank. Linked as oshcc links (§4.3), for its statics. tsan is
  # left out of the maps for shmem.internode.*'s reason.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    foreach(case "0,0,1,1" "0,1,0,1" "0,0,1,1,2,2" "0,1,2,0,1,2" "0,0,1,1,2,2,3,3"
                 "0,1,2,3,0,1,2,3" "0,0,1,1,2,2,3,3,4,4:OSOE_TEST_THREE_OF_FIVE=3,1,5"
                 "0,1,2,3,4,0,1,2,3,4:OSOE_TEST_THREE_OF_FIVE=2,3,3" "0,1,2,0,1,0")
      string(REPLACE ":" ";" environment "${case}")
      list(POP_FRONT environment map)
      osoe_tier2_map_test(allgather ${map} OSOE_TEST_MAP=${map} SHMEM_SYMMETRIC_SIZE=16m
        ${environment})
    endforeach()
    osoe_statics_program(shmem_test_allgather)
  endif()
endif()

# The white-box programs below take the library's own sources from its target,
# so each build's, and link liburing only where the build has it
# (OSOE_LIBURING_LINK, cmake/OsoeInternode.cmake): their one-node tests run in
# both builds, their tests across nodes in the full build alone.
#
# OSOE_REGION_INTERNAL itself (test-internal-region.c): between node-mates, a
# path no collective takes, across nodes, and both. White-box: it calls the
# transport, so the library's own sources are compiled into it.
get_target_property(osoe_library_sources osoe SOURCES)
list(TRANSFORM osoe_library_sources PREPEND ${PROJECT_SOURCE_DIR}/shmem/)
add_executable(shmem_test_internal-region test-internal-region.c ${osoe_library_sources})
target_include_directories(shmem_test_internal-region PRIVATE ../src ../include)
target_link_libraries(shmem_test_internal-region PRIVATE osoe_base rt Threads::Threads
  ${OSOE_LIBURING_LINK})
osoe_warnings(shmem_test_internal-region)
add_test(NAME shmem.internal-region.L2
  COMMAND osoe-launch -n 2 -t 60 -- $<TARGET_FILE:shmem_test_internal-region>)
set_tests_properties(shmem.internal-region.L2 PROPERTIES LABELS "tier2" TIMEOUT 90)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(internal-region 0,1)
  osoe_tier2_map_test(internal-region 0,0,1,1)
endif()

# Where a team's state lives and how its shape is found (phase 1 step 7 slice
# 1; test-team-storage.c): the descriptor's local list against the map - every
# local id in order, local id 0 the lead - SHMEM_TEAM_SHARED's world mapping
# both ways, a reduction, a gather and a broadcast on each team, and osoe-stat
# naming the teams by their records on the live job, with the world's record
# as the map implies it and the finalize team's summary line. White-box, like
# internal-region's. The map where local ids and world ranks interleave; 0,0,1,1,
# where node 1's lead is rank 2, so a record naming a node by its number is
# caught; SHARED as the PE alone under force-remote, whose list is a local id
# that is not 0; and a node whose local ids are given by hand against its ranks
# - rank 0 at local id 1 - on one node and on a node of two, where the generic
# internode phase meets the node at its local id 0, rank 1 (OSOE_TEST_LEADS).
add_executable(shmem_test_team-storage test-team-storage.c ${osoe_library_sources})
target_include_directories(shmem_test_team-storage PRIVATE ../src ../include)
# The split's parent sync, tested by delaying one node's record: team.c's call
# to the tree function goes through the test's wrapper (check_parent_sync).
target_link_options(shmem_test_team-storage PRIVATE "LINKER:--wrap=osoe_collective_tree")
target_link_libraries(shmem_test_team-storage PRIVATE osoe_base rt Threads::Threads
  ${OSOE_LIBURING_LINK})
osoe_warnings(shmem_test_team-storage)
set(osoe_hand_order -e 0:OSOE_LOCALID=1 -e 1:OSOE_LOCALID=0 -e 0:OSOE_GTIDS=1,0
    -e 1:OSOE_GTIDS=1,0)
add_test(NAME shmem.team-storage.local-order
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 ${osoe_hand_order} --
          $<TARGET_FILE:shmem_test_team-storage>)
set_tests_properties(shmem.team-storage.local-order PROPERTIES LABELS "tier2" TIMEOUT 90)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(team-storage 0,1,2,0,1,0 OSOE_TEST_MAP=0,1,2,0,1,0
    OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>)
  osoe_tier2_map_test(team-storage 0,0,1,1 OSOE_TEST_MAP=0,0,1,1
    OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>)
  if(OSOE_INTERNODE)
    add_test(NAME shmem.team-storage.force-remote.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1 OSOE_TEST_MAP=0,0,1,1
              OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
              $<TARGET_FILE:shmem_test_team-storage>)
    set_tests_properties(shmem.team-storage.force-remote.0-0-1-1 PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    add_test(NAME shmem.team-storage.local-order.internode
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MAP=0,0,1 OSOE_TEST_LEADS=1,2
              OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
              $<TARGET_FILE:osoe-launch> -n 3 -m 0,0,1 -t 60 ${osoe_hand_order} --
              $<TARGET_FILE:shmem_test_team-storage>)
    set_tests_properties(shmem.team-storage.local-order.internode PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_internode_label(shmem.team-storage.force-remote.0-0-1-1
      shmem.team-storage.local-order.internode)
  endif()
endif()

if(OSOE_INTERNODE)
  # The generic gather's own checks, each ending the job by name before a byte
  # of the gather lands (test-gather-misuse.c; phase 1 step 5 slice 6): a collect
  # into a dest shorter than its result, SOS's shmem_team_b2b_collectives'
  # mistake, whose lead's own copy of its members' blocks overwrites the room
  # holding the places; and a lead's descriptor counting one member more on its
  # node than the rank map does. White-box, for the second, like team-storage's.
  add_executable(shmem_test_gather-misuse test-gather-misuse.c ${osoe_library_sources})
  target_include_directories(shmem_test_gather-misuse PRIVATE ../src ../include)
  target_link_libraries(shmem_test_gather-misuse PRIVATE osoe_base rt Threads::Threads
    PkgConfig::LIBURING)
  osoe_warnings(shmem_test_gather-misuse)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.gather-misuse.short-dest
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MISUSE=short-dest
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 --
              $<TARGET_FILE:shmem_test_gather-misuse>)
    set_tests_properties(shmem.gather-misuse.short-dest PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "shmem_long_collect: dest has room for [0-9]+ bytes to the end of its allocation, and the result is 320 bytes")
    add_test(NAME shmem.gather-misuse.local-count
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MISUSE=local-count
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 --
              $<TARGET_FILE:shmem_test_gather-misuse>)
    set_tests_properties(shmem.gather-misuse.local-count PROPERTIES LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "test-gather-misuse: the rank map puts 2 of team 0's members on this node, and the team lists 3 here")
    osoe_internode_label(shmem.gather-misuse.short-dest shmem.gather-misuse.local-count)
  endif()
endif()
# Team creation (phase 1 step 7 slice 2; test-team-split.c): halves, evens and
# odds, one PE, the world reversed, grids of the world and of a split, SHARED
# split, splits of splits, and two teams whose local leads coincide running
# collectives back to back - each team's shape and every collective on it
# checked, with the arrival table showing no team counting another's arrivals.
# On one node at 1, 2 and 4 PEs; on a block map, an interleaved one and four
# nodes of one PE, where {0, 1} and {0, 2} meet at PE 0 from two other nodes;
# and under force-remote, where each PE's SHARED is itself. The program makes
# about forty teams, destroying each as it goes, so that its numbers are given
# back and taken again, and their blocks with them (the team redesign's step D).
add_executable(shmem_test_team-split test-team-split.c
  ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
target_include_directories(shmem_test_team-split PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
target_link_libraries(shmem_test_team-split PRIVATE osoe rt)
osoe_warnings(shmem_test_team-split)
set(osoe_team_split_environment SHMEM_SYMMETRIC_SIZE=16m)
foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
  add_test(NAME shmem.team-split.L${count}
    COMMAND ${CMAKE_COMMAND} -E env ${osoe_team_split_environment}
            $<TARGET_FILE:osoe-launch> -n ${count} -t 60 -- $<TARGET_FILE:shmem_test_team-split>)
  set_tests_properties(shmem.team-split.L${count} PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# What a split needs and what a number is, each a mode of the program (the
# program's comment), judged by what PE 0 says rather than by the exit status -
# so every failure a run can show fails it by its words: exhaust, under
# OSOE_MAX_TEAMS=8, where two teams with no PE in common hold one number and a
# split_2d with one number free fails, then the world again with none free on
# PE 0, twice, and takes the two teams' number once both are destroyed;
# too-many, under OSOE_MAX_TEAMS=4, a split_2d, which needs two numbers,
# failing; dry, under OSOE_TEAM_BLOCKS=8 on one node, two splits finding the
# pool dry, PE 0 naming the setting and its node each time, a split_2d needing
# three blocks where one is free failing too - not running its rounds for
# ever, holding the one it claimed - and a destroyed team's block serving the
# next; stale-handle, a destroyed team's handle used
# once its number names another team, which ends the job by name; reuse, a
# team taking a destroyed team's block under another name; and full, the heap
# filled to its last block under SHMEM_SYMMETRIC_SIZE=1m.
function(osoe_team_split_mode mode label launch environment pass)
  add_test(NAME shmem.team-split.${mode}.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_SPLIT=${mode} ${environment}
            $<TARGET_FILE:osoe-launch> ${launch} -t 60 -- $<TARGET_FILE:shmem_test_team-split>)
  set_tests_properties(shmem.team-split.${mode}.${label} PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "${pass}"
    FAIL_REGULAR_EXPRESSION "CHECK[(]|exited with status|killed by signal|timeout after")
endfunction()
set(osoe_team_split_exhaust_environment OSOE_MAX_TEAMS=8 SHMEM_SYMMETRIC_SIZE=16m)
set(osoe_team_split_exhausted
  "PE 0: shmem_team_split_2d: no two team numbers are free on every PE of the new teams - between them they hold all but at most one of those a program may use, though each may hold fewer: OSOE_MAX_TEAMS is 8, the teams one PE may be in at once.*PE 0: shmem_team_split_strided: no team number is free on every PE of the new team - between them they hold every one a program may use, though each may hold fewer: OSOE_MAX_TEAMS is 8.*PE 0: shmem_team_split_strided: no team number is free on every PE of the new team - between them.*OSOE_MAX_TEAMS is 8.*every check of this mode passed")
set(osoe_team_split_full "every check of this mode passed")
osoe_team_split_mode(exhaust L4 "-n;4" "${osoe_team_split_exhaust_environment}"
  "${osoe_team_split_exhausted}")
osoe_team_split_mode(too-many L4 "-n;4" "OSOE_MAX_TEAMS=4;SHMEM_SYMMETRIC_SIZE=16m"
  "PE 0: shmem_team_split_2d: no two team numbers are free on every PE of the new teams - between them.*OSOE_MAX_TEAMS is 4.*PE 0: shmem_team_split_strided: no team number is free on every PE of the new team - between them.*OSOE_MAX_TEAMS is 4.*every check of this mode passed")
osoe_team_split_mode(dry L2 "-n;2" "OSOE_TEAM_BLOCKS=8;SHMEM_SYMMETRIC_SIZE=16m"
  "PE 0: shmem_team_split_strided: node 0 [(][^)]*[)] has no team block left for the new team this PE would lead there: OSOE_TEAM_BLOCKS is 8, the teams a node may hold at once.*PE 0: shmem_team_split_strided: node 0 [(][^)]*[)] has no team block left.*PE 0: shmem_team_split_2d: node 0 [(][^)]*[)] has no team block left.*every check of this mode passed")
osoe_team_split_mode(full L4 "-n;4" "SHMEM_SYMMETRIC_SIZE=1m" "${osoe_team_split_full}")
osoe_team_split_mode(reuse L4 "-n;4" "SHMEM_SYMMETRIC_SIZE=16m" "${osoe_team_split_full}")
# The stale handle's pass is the job's end, named: its own registration, since
# the failure expression above would take that end for a failure.
add_test(NAME shmem.team-split.stale-handle.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_SPLIT=stale-handle SHMEM_SYMMETRIC_SIZE=16m
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_team-split>)
set_tests_properties(shmem.team-split.stale-handle.L2 PROPERTIES LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "every check before the stale handle passed.*shmem_team_my_pe: 0x[0-9a-f]+ is team 3, which this PE has destroyed; the number names another team since"
  FAIL_REGULAR_EXPRESSION "CHECK[(]")
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    osoe_tier2_map_test(team-split 0,0,1,1 ${osoe_team_split_environment})
    osoe_tier2_map_test(team-split 0,1,2,0,1,0 ${osoe_team_split_environment})
    osoe_tier2_map_test(team-split 0,1,2,3 ${osoe_team_split_environment})
    add_test(NAME shmem.team-split.force-remote.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1 ${osoe_team_split_environment}
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 --
              $<TARGET_FILE:shmem_test_team-split>)
    set_tests_properties(shmem.team-split.force-remote.0-0-1-1 PROPERTIES LABELS "tier2"
      TIMEOUT 90)
    osoe_team_split_mode(exhaust 0-0-1-1 "-n;4;-m;0,0,1,1" "${osoe_team_split_exhaust_environment}"
      "${osoe_team_split_exhausted}")
    osoe_team_split_mode(full 0-0-1-1 "-n;4;-m;0,0,1,1" "SHMEM_SYMMETRIC_SIZE=1m"
      "${osoe_team_split_full}")
    osoe_team_split_mode(reuse 0-0-1-1 "-n;4;-m;0,0,1,1" "SHMEM_SYMMETRIC_SIZE=16m"
      "${osoe_team_split_full}")
    osoe_team_split_mode(reuse 0-1-2-0-1-0 "-n;6;-m;0,1,2,0,1,0" "SHMEM_SYMMETRIC_SIZE=16m"
      "${osoe_team_split_full}")
    osoe_internode_label(shmem.team-split.force-remote.0-0-1-1 shmem.team-split.exhaust.0-0-1-1
      shmem.team-split.full.0-0-1-1 shmem.team-split.reuse.0-0-1-1
      shmem.team-split.reuse.0-1-2-0-1-0)
  endif()
endif()

# OSOE_TEAM_BLOCKS (§5.7; the team redesign's step D): a node's pool of team
# blocks, which its lead lays out, so the PEs of a node must agree - refused as
# OSOE_MAX_TEAMS is - while two nodes may hold pools of different sizes, since
# the wire names a team by its name on the node it goes to and never by a block
# index: the team split program, whose forty teams span both nodes of the
# second map, passes with its second node's pool a tenth of its first's. A
# pool too small for the teams a node holds makes the split fail alike, which
# the split program's dry mode tests (above).
osoe_refusal_test(shmem.team-blocks.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_TEAM_BLOCKS=64
  PATTERNS "osoe: PE 1: OSOE_TEAM_BLOCKS differs between PEs: the local lead has 1024, this PE has 64"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
foreach(value 7 65537 x)
  add_test(NAME shmem.team-blocks.invalid.${value}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEAM_BLOCKS=${value}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.team-blocks.invalid.${value} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "OSOE_TEAM_BLOCKS='${value}' is not between 8 and 65536")
endforeach()
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.job.disagree.team-blocks
      COMMAND ${CMAKE_COMMAND} -E env ${osoe_team_split_environment}
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -e 2:OSOE_TEAM_BLOCKS=100
              -e 3:OSOE_TEAM_BLOCKS=100 -t 60 -- $<TARGET_FILE:shmem_test_team-split>)
    set_tests_properties(shmem.job.disagree.team-blocks PROPERTIES LABELS "tier2" TIMEOUT 90)
    osoe_internode_label(shmem.job.disagree.team-blocks)
  endif()
endif()

set(osoe_get_nbi_small OSOE_GET_LANDING_BYTES=4096
    OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(get-nbi 0,0,1,1 ${osoe_get_nbi_small})
  # A blocking get of more than a piece issues its pieces together, on its
  # thread's own context, and copies each out as it lands
  # (docs/decisions/2026-10-10-blocking-gets.md; test-get-pieces.c): a get of a
  # mebibyte moves am.gets_completed by a piece at least twice, one of a piece
  # not at all, and its pieces fill the zone before any is copied out. The
  # failure expression, since a pass expression makes CTest ignore the exit
  # status, and the banner comes before the invariants' check and finalize.
  # A job of two nodes: in the single-node build the test is not registered,
  # and a property on it would refuse that build at configure time.
  if(OSOE_INTERNODE)
    osoe_tier2_map_test(get-pieces 0,1)
    set_tests_properties(shmem.get-pieces.0-1 PROPERTIES PASS_REGULAR_EXPRESSION "GET PIECES OK"
      FAIL_REGULAR_EXPRESSION "CHECK[(]|invariants:|osoe-launch: (rank|timeout)")
    # And the fallback: at OSOE_MAX_CONTEXTS 4096 every thread place's block is
    # past what a token names, so the get goes a piece at a time, as before the
    # change - the one path no other test takes since.
    add_test(NAME shmem.get-pieces.fallback.0-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_GET_PIECES=fallback OSOE_MAX_CONTEXTS=4096
              $<TARGET_FILE:osoe-launch> -n 2 -m 0,1 -t 60 -- $<TARGET_FILE:shmem_test_get-pieces>)
    set_tests_properties(shmem.get-pieces.fallback.0-1 PROPERTIES LABELS "tier2" TIMEOUT 90
      PASS_REGULAR_EXPRESSION "GET PIECES FALLBACK OK"
      FAIL_REGULAR_EXPRESSION "CHECK[(]|invariants:|osoe-launch: (rank|timeout)")
    osoe_internode_label(shmem.get-pieces.fallback.0-1)
  endif()
  osoe_tier2_map_test(get-nbi 0,1,2,0,1,0 ${osoe_get_nbi_small})
endif()

# SHMEM_THREAD_MULTIPLE (design §4.11; phase 1 step 8 slice 2;
# test-multiple.c): worker threads of every PE in the library at once, on
# private contexts and on the default one, waiting several at a time; on one
# node, and across nodes on a block and an interleaved map, with a landing zone
# of 8 extents that each thread's 12 pieces in flight overfill, and that each
# thread's blocking get of 20 KiB takes in three pieces, copied out by whichever
# thread finds them landed; and a full thread table ending the job by name. Under tsan the cross-node case that
# counts is 0,1: one PE a node, so everything that writes a PE's memory - its
# agent's landings, its threads' copy-outs, its replies - is a thread of the
# one process tsan instruments, and a race among them is a report, not the
# unprovable cross-process kind the internode tests are kept from.
osoe_tier2_test(multiple)
osoe_tier2_map_test(multiple 0,1 OSOE_GET_LANDING_BYTES=8192)
if(OSOE_INTERNODE)
  # With the library checking every shared wait's promise at its return and
  # perturbing the steps of its protocols (OSOE_CHECK_QUIET, OSOE_TEST_PERTURB;
  # transport-internode.c), so that a race the models find happens and shows.
  foreach(map IN ITEMS 0,1 0,1,2,0,1,0)
    string(REPLACE "," ";" ranks "${map}")
    list(LENGTH ranks count)
    string(REPLACE "," "-" label "${map}")
    if(map STREQUAL "0,1" OR NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
      add_test(NAME shmem.multiple.checked.${label}
        COMMAND ${CMAKE_COMMAND} -E env OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1
                OSOE_GET_LANDING_BYTES=8192
                $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 120 --
                $<TARGET_FILE:shmem_test_multiple>)
      set_tests_properties(shmem.multiple.checked.${label} PROPERTIES LABELS "tier2" TIMEOUT 180
        FAIL_REGULAR_EXPRESSION "OSOE_CHECK_QUIET:")
      osoe_internode_label(shmem.multiple.checked.${label})
    endif()
  endforeach()
endif()
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(multiple 0,0,1,1 OSOE_GET_LANDING_BYTES=8192)
  osoe_tier2_map_test(multiple 0,1,2,0,1,0 OSOE_GET_LANDING_BYTES=8192)
endif()
add_test(NAME shmem.multiple.exhaust
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_MULTIPLE=exhaust OSOE_MAX_THREADS=2
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_multiple>)
set_tests_properties(shmem.multiple.exhaust PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "more live threads of this PE have called the library than OSOE_MAX_THREADS=2 allows")
target_link_libraries(shmem_test_multiple PRIVATE Threads::Threads)

# Two splits at once on one PE under SHMEM_THREAD_MULTIPLE (the team
# redesign's step D, D7, as redesigned; test-team-threads.c): two threads of
# every PE splitting together, both succeeding; and on three PEs or more the
# adversarial read's program, which the first design's mask token deadlocked,
# finishing, its split of {0, 2} losing a round to {0, 1}'s number - PE 0's
# shmem.team_split.lost_rounds says so. On one node, and on 0,1 and 0,1,2,
# which tsan sees whole, one PE a node - the second the read's program; and
# 0,0,1,1.
osoe_tier2_test(team-threads)
target_link_libraries(shmem_test_team-threads PRIVATE Threads::Threads)
osoe_tier2_map_test(team-threads 0,1)
osoe_tier2_map_test(team-threads 0,1,2)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(team-threads 0,0,1,1)
endif()

# A team's destroy on one thread while another creates and destroys contexts,
# under SHMEM_THREAD_MULTIPLE (test-context-team-destroy.c): the destroy's scan
# of the context pool now holds the pool's mutex, which the tsan preset shows
# missing otherwise (the review of step D's merge point, conformance F1).
osoe_tier2_test(context-team-destroy)
target_link_libraries(shmem_test_context-team-destroy PRIVATE Threads::Threads)

# Splits whose parent PEs ask for different thread levels (test-team-levels.c):
# PE 0 SHMEM_THREAD_MULTIPLE, the rest shmem_init's SERIALIZED, every parent PE
# running the same rounds because the first reduction's marks say whether any
# has the level - the adversarial read of the rebuilt step D part 2 found a
# split that never finished when each PE decided by its own. On one node, and
# on 0,1 and 0,0,1,1.
osoe_tier2_test(team-levels)
osoe_tier2_map_test(team-levels 0,1)
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  osoe_tier2_map_test(team-levels 0,0,1,1)
endif()
# A landing zone that is not whole extents is refused by name, not rounded.
add_test(NAME shmem.multiple.landing-unaligned
  COMMAND ${CMAKE_COMMAND} -E env OSOE_GET_LANDING_BYTES=1500
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_multiple>)
set_tests_properties(shmem.multiple.landing-unaligned PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_GET_LANDING_BYTES=1500 is not a multiple of the landing zone's 1024-byte extents")

# What OSOE_MAX_THREADS counts (test-thread-places.c): the live threads of a PE
# that have called the library, not the threads in a call at once. A worker that
# made one call and lives on holds its place, so the next one's first call ends
# the job though no two were ever in the library together; and an exit gives a
# place back, so many workers one after another need only one. The exhaust test
# above parks its workers in waits, which fits either reading.
add_executable(shmem_test_thread-places test-thread-places.c)
target_link_libraries(shmem_test_thread-places PRIVATE osoe Threads::Threads)
osoe_warnings(shmem_test_thread-places)
add_test(NAME shmem.thread-places.held
  COMMAND ${CMAKE_COMMAND} -E env OSOE_MAX_THREADS=2
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_thread-places> held)
set_tests_properties(shmem.thread-places.held PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "a worker made one call and lives on.*more live threads of this PE have called the library than OSOE_MAX_THREADS=2 allows")
add_test(NAME shmem.thread-places.freed
  COMMAND ${CMAKE_COMMAND} -E env OSOE_MAX_THREADS=2
          $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_thread-places> freed)
set_tests_properties(shmem.thread-places.freed PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "20 workers each made one call and exited"
  FAIL_REGULAR_EXPRESSION "more live threads of this PE")

# The agent's scheduler slice (§3.11; test-agent-slice.c): the default, a set
# one, the kernel's own, and a value out of range refused by name.
osoe_tier2_test(agent-slice)
add_test(NAME shmem.agent-slice.set.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_SLICE_US=2500
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_agent-slice>)
add_test(NAME shmem.agent-slice.kernel.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_SLICE_US=0
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_agent-slice>)
add_test(NAME shmem.agent-slice.refused.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_SLICE_US=50
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_agent-slice>)
set_tests_properties(shmem.agent-slice.set.L2 shmem.agent-slice.kernel.L2 PROPERTIES
  LABELS "tier2" TIMEOUT 90)
set_tests_properties(shmem.agent-slice.refused.L2 PROPERTIES LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "OSOE_AGENT_SLICE_US=50: 0, or 100 to 100000 microseconds")

# Where each agent may run (agent.c; test-agent-cpus.c): every PE bound to
# one CPU before init, as Slurm binds tasks that fill a node, and the agent on
# its own PE's CPU by default, on its node's PEs' CPUs with OSOE_AGENT_CPUS=node;
# a value it does not know is refused by name.
add_executable(shmem_test_agent-cpus test-agent-cpus.c)
target_link_libraries(shmem_test_agent-cpus PRIVATE osoe)
osoe_warnings(shmem_test_agent-cpus)
if(OSOE_INTERNODE)
  add_test(NAME shmem.agent-cpus.task.0-0-1-1
    COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 60 -- $<TARGET_FILE:shmem_test_agent-cpus>)
  add_test(NAME shmem.agent-cpus.node.0-0-1-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_CPUS=node
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 -- $<TARGET_FILE:shmem_test_agent-cpus>)
endif()
add_test(NAME shmem.agent-cpus.node.L3
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_CPUS=node
          $<TARGET_FILE:osoe-launch> -n 3 -t 60 -- $<TARGET_FILE:shmem_test_agent-cpus>)
set_tests_properties(shmem.agent-cpus.node.L3 PROPERTIES LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "AGENT CPUS OK" FAIL_REGULAR_EXPRESSION "CHECK[(]")
if(OSOE_INTERNODE)
  set_tests_properties(shmem.agent-cpus.task.0-0-1-1 shmem.agent-cpus.node.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "AGENT CPUS OK" FAIL_REGULAR_EXPRESSION "CHECK[(]")
  osoe_internode_label(shmem.agent-cpus.task.0-0-1-1 shmem.agent-cpus.node.0-0-1-1)
endif()
add_test(NAME shmem.agent-cpus.refused.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_AGENT_CPUS=all
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_agent-cpus>)
set_tests_properties(shmem.agent-cpus.refused.L2 PROPERTIES LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "OSOE_AGENT_CPUS=all: task, for its own PE's CPUs, or node")

# The deadlock detector (§3.15, deadlock.h; test-deadlock.c), at
# OSOE_DEADLOCK_SECONDS=1: a job deadlocked on one node, across two, in a
# second session after a finalize, and on a collective that does not match
# across two, ended naming where its PEs wait;
# and a job that only looks deadlocked - its last PE computing, asleep outside
# the library, or waiting for a thread of its own that the library cannot see,
# or every PE waiting while a thread of its own computes - on one node and
# across two, run to its end. Off, a deadlock runs to the
# launcher's timeout; a setting it cannot read is refused by name.
#
# ThreadSanitizer runs a thread of its own in every process, which the detector
# counts, rightly, as a thread the library cannot see: under it no job is
# judged deadlocked, so the four that must be are not run there.
add_executable(shmem_test_deadlock test-deadlock.c)
target_link_libraries(shmem_test_deadlock PRIVATE osoe)
osoe_warnings(shmem_test_deadlock)
# On one node as well as across two, each but the interleaved map's mismatch:
# the one-node detector's half, which a desktop job takes, ran on two nodes
# alone (the v0.2 step 1 review, correctness K1).
set(deadlock_cases
    "waits|0,0,0,0|in shmem_int_wait_until [(]heap [+][0-9a-fx]+[)], for [0-9]+ s"
    "waits|0,0,1,1|in shmem_int_wait_until [(]heap [+][0-9a-fx]+[)], for [0-9]+ s"
    "reinit|0,0,0,0|in shmem_int_wait_until [(]heap [+][0-9a-fx]+[)], for [0-9]+ s"
    "reinit|0,0,1,1|in shmem_int_wait_until [(]heap [+][0-9a-fx]+[)], for [0-9]+ s"
    "mismatch|0,0,0,0|in shmem_barrier_all [(]epoch [0-9]+[)], for [0-9]+ s"
    "mismatch|0,0,1,1|in shmem_barrier_all [(]epoch [0-9]+[)], for [0-9]+ s"
    "mismatch|0,1,0,1|in shmem_barrier_all [(]epoch [0-9]+[)], for [0-9]+ s")
if(CMAKE_C_FLAGS MATCHES "sanitize=thread")
  set(deadlock_cases "")
endif()
foreach(case IN LISTS deadlock_cases)
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 mode)
  list(GET parts 1 map)
  list(GET parts 2 where)
  string(REPLACE "," "-" label "${map}")
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.deadlock.${mode}.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=1
            $<TARGET_FILE:osoe-launch> -n 4 -m ${map} -t 30 --
            $<TARGET_FILE:shmem_test_deadlock> ${mode})
  set_tests_properties(shmem.deadlock.${mode}.${label} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "every PE of the job waits in the library.*: PE [0-9, ]+ ${where}"
    FAIL_REGULAR_EXPRESSION "DEADLOCK TEST REACHED|a wait it did not record|osoe-launch: timeout|CHECK[(]")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.deadlock.${mode}.${label})
  endif()
endforeach()
foreach(case "computing|0,0,0,0" "computing|0,0,1,1" "sleeping|0,0,0,0" "sleeping|0,0,1,1"
             "helper|0,0,0,0"
             "helper|0,0,1,1" "busy|0,0,0,0" "busy|0,0,1,1")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 mode)
  list(GET parts 1 map)
  string(REPLACE "," "-" label "${map}")
  string(REPLACE "," ";" ranks "${map}")
  list(LENGTH ranks count)
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.deadlock.${mode}.${label}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=1
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_deadlock> ${mode})
  set_tests_properties(shmem.deadlock.${mode}.${label} PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "DEADLOCK TEST OK: ${mode}"
    FAIL_REGULAR_EXPRESSION "osoe: deadlock|CHECK[(]")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.deadlock.${mode}.${label})
  endif()
endforeach()
add_test(NAME shmem.deadlock.off.0-0-0-0
  COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=0
          $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,0,0 -t 6 --
          $<TARGET_FILE:shmem_test_deadlock> waits)
set_tests_properties(shmem.deadlock.off.0-0-0-0 PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe-launch: timeout after 6 s"
  FAIL_REGULAR_EXPRESSION "osoe: deadlock|DEADLOCK TEST REACHED")
if(OSOE_INTERNODE)
  add_test(NAME shmem.deadlock.off.0-0-1-1
    COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=0
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 6 --
            $<TARGET_FILE:shmem_test_deadlock> waits)
  set_tests_properties(shmem.deadlock.off.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "osoe-launch: timeout after 6 s"
    FAIL_REGULAR_EXPRESSION "osoe: deadlock|DEADLOCK TEST REACHED")
  osoe_internode_label(shmem.deadlock.off.0-0-1-1)
endif()
add_test(NAME shmem.deadlock.refused.L2
  COMMAND ${CMAKE_COMMAND} -E env OSOE_DEADLOCK_SECONDS=soon
          $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- $<TARGET_FILE:shmem_test_deadlock> computing)
set_tests_properties(shmem.deadlock.refused.L2 PROPERTIES LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "OSOE_DEADLOCK_SECONDS=soon: 0 to turn deadlock detection off")

# The shape each large reduction and broadcast takes on the data path (v0.2
# step 2, Part 2; test-shapes-chosen.c): with the link set to the Pi's and to a
# loopback's, on 4, 16 and 64 logical nodes - 4 and 16 under a sanitizer - every
# call's fan-out and window the chooser's, held to shape-grid.txt, the model's,
# and its result right.
if(OSOE_INTERNODE AND NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_executable(shmem_test_shapes-chosen test-shapes-chosen.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_shapes-chosen PRIVATE ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_shapes-chosen PRIVATE osoe rt)
  osoe_warnings(shmem_test_shapes-chosen)
  foreach(case "pi;4;86000;7000;116000000" "pi;16;86000;7000;116000000"
               "loopback;16;20000;2000;4000000000" "pi;64;86000;7000;116000000"
               "loopback;64;20000;2000;4000000000")
    list(GET case 0 link)
    list(GET case 1 nodes)
    list(GET case 2 hop)
    list(GET case 3 message)
    list(GET case 4 bandwidth)
    # Sixty-four PEs under a sanitizer outgrow the VM's 8 GiB - the kernel
    # killed one under asan (Part 2's record) - so the sanitizers run 4 and 16.
    if(nodes GREATER 16 AND CMAKE_C_FLAGS MATCHES "sanitize=")
      continue()
    endif()
    set(map "0")
    math(EXPR last "${nodes} - 1")
    foreach(node RANGE 1 ${last})
      string(APPEND map ",${node}")
    endforeach()
    add_test(NAME shmem.shape.chosen.${link}.${nodes}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_LINK_HOP_NS=${hop} OSOE_LINK_MESSAGE_NS=${message}
              OSOE_LINK_BYTES_PER_SECOND=${bandwidth} SHMEM_SYMMETRIC_SIZE=4m
              $<TARGET_FILE:osoe-launch> -n ${nodes} -m ${map} -t 240 --
              $<TARGET_FILE:shmem_test_shapes-chosen> ${CMAKE_CURRENT_SOURCE_DIR}/shape-grid.txt)
    set_tests_properties(shmem.shape.chosen.${link}.${nodes} PROPERTIES LABELS "tier2"
      TIMEOUT 300 PASS_REGULAR_EXPRESSION "SHAPES CHOSEN: 12 calls checked"
      FAIL_REGULAR_EXPRESSION "CHECK[(]")
    osoe_internode_label(shmem.shape.chosen.${link}.${nodes})
  endforeach()

  # And back to back, no barrier between calls whose fan-outs differ, on the
  # world and two strided teams sharing its leads (test-shapes-back-to-back.c;
  # the step's adversarial read, finding 2): a large reduction's grants go to
  # this call's children. Sixteen nodes of one PE each, and eight of two, where
  # the odds' lead is local id 1. Since v0.2 step 3 Träff's reductions among
  # them, which the test counts and requires.
  add_executable(shmem_test_shapes-back-to-back test-shapes-back-to-back.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_shapes-back-to-back PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_shapes-back-to-back PRIVATE osoe rt)
  osoe_warnings(shmem_test_shapes-back-to-back)
  foreach(map "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15" "0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7")
    string(REPLACE "," "-" label "${map}")
    add_test(NAME shmem.shape.back-to-back.${label}
      COMMAND ${CMAKE_COMMAND} -E env OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000
              OSOE_LINK_BYTES_PER_SECOND=116000000 SHMEM_SYMMETRIC_SIZE=16m
              $<TARGET_FILE:osoe-launch> -n 16 -m ${map} -t 240 --
              $<TARGET_FILE:shmem_test_shapes-back-to-back> 40)
    set_tests_properties(shmem.shape.back-to-back.${label} PROPERTIES LABELS "tier2"
      TIMEOUT 300 PASS_REGULAR_EXPRESSION "SHAPES BACK TO BACK: 40 rounds on 16 PEs"
      FAIL_REGULAR_EXPRESSION "CHECK[(]")
    osoe_internode_label(shmem.shape.back-to-back.${label})
  endforeach()
endif()

# Träff's circulant reduce-scatter and allgather on the data path (v0.2 step 3,
# Part 2; test-circulant-reduce.c): the link set to the Pi's, so that the test
# knows what the chooser takes, over 2 to 70 logical nodes - the counts that
# are not powers of two among them by name - one PE a node, and over eight
# nodes of two: sizes across the chooser's crossover, every typed reduction at a
# size Träff's takes, in place, staggered entries, and calls back to back with
# small collectives and trees, each call's result and algorithm checked. Under
# OSOE_TEST_PERTURB on 5 and 16 nodes; two threads a PE reducing two teams at
# once on 4 and 16; under the AM stress settings, as shmem.pipeline.*.stress
# runs the trees; at the fewest team numbers and lanes, OSOE_MAX_TEAMS=4 and
# OSOE_MAX_THREADS=1, where the lanes end the region and a slot past a lane's
# rows runs off its end; under OSOE_COLLECTIVE_SHAPE=tree, where no call may
# take Träff's; with every node's lead lending a lane of its own (lanes); at a
# ceiling of 2, where five nodes' three rounds take every row of a lane, the
# release row's too; at a capacity of 1; at a window of 128 (a ceiling of 32
# and a capacity of 8 on three nodes); and on a link so slow that a few hundred
# bytes take Träff's, with fewer elements than nodes (tiny): empty blocks. And a
# divergent call, which must end by name (diverge). The sanitizers run the cases of up to 9 PEs - 12 and 16 under
# asan, beside the suite's other tests, were killed by the VM's OOM killer
# (asan's own memory, about 55 MB a PE; the Part 2 record) - and tsan one PE a
# node alone, as shmem.lanes.* does.
if(OSOE_INTERNODE)
  add_executable(shmem_test_circulant-reduce test-circulant-reduce.c
    ../src/collective-shape.c ../src/collective-circulant.c ../src/collective-tree.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_circulant-reduce PRIVATE ../src
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_circulant-reduce PRIVATE osoe rt Threads::Threads)
  osoe_warnings(shmem_test_circulant-reduce)
  foreach(case 2 3 4 5 6 7 8 9 12 16 17 33 64 70 8x2 5.perturb 16.perturb 4.threads
               16.threads 5.stress 4.max-teams-4 16.tree 7.lanes 5.lanes 5.ceiling-2
               5.capacity-1 3.window-128 16.tiny)
    string(REGEX MATCH "^[0-9]+" nodes "${case}")
    set(per_node 1)
    if(case MATCHES "x2$")
      set(per_node 2)
    endif()
    set(extra "")
    set(arguments "")
    set(link OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000
             OSOE_LINK_BYTES_PER_SECOND=116000000)
    if(case MATCHES "perturb")
      set(extra OSOE_TEST_PERTURB=1)
    elseif(case MATCHES "threads")
      set(arguments threads)
    elseif(case MATCHES "stress")
      set(extra OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2 OSOE_WIRE_BYTES=256
                OSOE_WIRE_SPLIT=2000 OSOE_WIRE_SEED=20261007)
    elseif(case MATCHES "max-teams-4")
      set(extra OSOE_MAX_TEAMS=4 OSOE_MAX_THREADS=1)
    elseif(case MATCHES "tree")
      set(extra OSOE_COLLECTIVE_SHAPE=tree)
    elseif(case MATCHES "lanes")
      set(arguments lanes)
    elseif(case MATCHES "ceiling-2")
      set(extra OSOE_COLLECTIVE_FANOUT=2)
    elseif(case MATCHES "capacity-1")
      set(extra OSOE_COLLECTIVE_PIPELINE=1)
    elseif(case MATCHES "window-128")
      set(extra OSOE_COLLECTIVE_FANOUT=32 OSOE_COLLECTIVE_PIPELINE=8)
    elseif(case MATCHES "tiny")
      set(link OSOE_LINK_HOP_NS=1000 OSOE_LINK_MESSAGE_NS=0 OSOE_LINK_BYTES_PER_SECOND=1000)
      set(extra OSOE_SMALL_COLLECTIVE_BYTES=128)
    endif()
    math(EXPR pes "${nodes} * ${per_node}")
    if(pes GREATER 9 AND CMAKE_C_FLAGS MATCHES "sanitize=")
      continue()
    endif()
    if(per_node GREATER 1 AND CMAKE_C_FLAGS MATCHES "sanitize=thread")
      continue()
    endif()
    set(map "")
    math(EXPR last "${nodes} - 1")
    foreach(node RANGE 0 ${last})
      foreach(copy RANGE 1 ${per_node})
        if(map STREQUAL "")
          set(map "${node}")
        else()
          string(APPEND map ",${node}")
        endif()
      endforeach()
    endforeach()
    set(timeout 300)
    if(nodes GREATER 16)
      set(timeout 900)
    endif()
    math(EXPR launch_timeout "${timeout} - 60")
    add_test(NAME shmem.circulant.${case}
      COMMAND ${CMAKE_COMMAND} -E env ${link} SHMEM_SYMMETRIC_SIZE=12m ${extra}
              $<TARGET_FILE:osoe-launch> -n ${pes} -m ${map} -t ${launch_timeout} --
              $<TARGET_FILE:shmem_test_circulant-reduce> ${arguments})
    set_tests_properties(shmem.circulant.${case} PROPERTIES LABELS "tier2"
      TIMEOUT ${timeout} PASS_REGULAR_EXPRESSION "CIRCULANT REDUCE: .* on ${nodes} nodes"
      FAIL_REGULAR_EXPRESSION "CHECK[(]")
    osoe_internode_label(shmem.circulant.${case})
  endforeach()
  # Two nodes' leads in Träff's reductions of different bytes: a lead's grant
  # field from the receiver that differs says so, and the job ends naming that
  # receiver's lead and round - matched by the field check's own words, since a
  # piece's header ends a job with the same closing phrase a round later, and
  # with the field check removed this test passed on it (the grant fields'
  # adversarial read, finding 1).
  add_test(NAME shmem.circulant.4.diverge
    COMMAND ${CMAKE_COMMAND} -E env OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000
            OSOE_LINK_BYTES_PER_SECOND=116000000
            $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 60 --
            $<TARGET_FILE:shmem_test_circulant-reduce> diverge)
  set_tests_properties(shmem.circulant.4.diverge PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "the reduce-scatter grant of node [0-9]+'s lead \\(PE [0-9]+\\), which this node's lead \\(PE [0-9]+\\) sends round [0-9]+, is for"
    FAIL_REGULAR_EXPRESSION "a divergent reduction returned")
  osoe_internode_label(shmem.circulant.4.diverge)
endif()

# The prefix scans (spec §9.10.10; phase 1 step 10 slice 1; test-scan.c): on
# one node, where the team's local lead scans; across nodes on a block, an
# interleaved and an uneven map, and under force-remote, where every member
# runs its place in the scan's tree; and on five nodes at fan-out 2, where
# that tree over ten members is three levels deep, with the AM layer's small
# settings, which split a scan's pieces on the wire.
osoe_tier2_test(scan)
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    osoe_tier2_map_test(scan 0,1,2,3)
    osoe_tier2_map_test(scan 0,0,1,1)
    osoe_tier2_map_test(scan 0,1,2,0,1,0)
    osoe_tier2_map_test(scan 0,0,0,1,2,2)
    add_test(NAME shmem.scan.force-remote.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 -- $<TARGET_FILE:shmem_test_scan>)
    set_tests_properties(shmem.scan.force-remote.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 90)
    add_test(NAME shmem.scan.fanout-2.0-1-2-3-4-0-1-2-3-4
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_FANOUT=2 OSOE_AM_CHUNK=1024
              OSOE_AM_BUFFER=2048 OSOE_AM_CREDITS=2
              $<TARGET_FILE:osoe-launch> -n 10 -m 0,1,2,3,4,0,1,2,3,4 -t 90 --
              $<TARGET_FILE:shmem_test_scan>)
    set_tests_properties(shmem.scan.fanout-2.0-1-2-3-4-0-1-2-3-4 PROPERTIES LABELS "tier2"
      TIMEOUT 120)
    # Pieces in flight (2026-09-27): one at a time, as first built, and four on
    # the three-level tree, the leaves' counts pinned by the test; the most the
    # agreement allows, eight, on four nodes. Four and eight under
    # OSOE_COLLECTIVE_SHAPE=fixed, where a scan keeps the capacity in flight -
    # under link it keeps 0.1.0's two (v0.2 step 2) - and the scan's fixed path
    # with them, which nothing else runs (the step's adversarial read).
    add_test(NAME shmem.scan.pipeline-1.fanout-2.0-1-2-3-4-0-1-2-3-4
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_PIPELINE=1 OSOE_COLLECTIVE_FANOUT=2
              $<TARGET_FILE:osoe-launch> -n 10 -m 0,1,2,3,4,0,1,2,3,4 -t 90 --
              $<TARGET_FILE:shmem_test_scan>)
    add_test(NAME shmem.scan.pipeline-4.fanout-2.0-1-2-3-4-0-1-2-3-4
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_PIPELINE=4
              OSOE_COLLECTIVE_FANOUT=2
              $<TARGET_FILE:osoe-launch> -n 10 -m 0,1,2,3,4,0,1,2,3,4 -t 90 --
              $<TARGET_FILE:shmem_test_scan>)
    add_test(NAME shmem.scan.pipeline-8.0-1-2-3
      COMMAND ${CMAKE_COMMAND} -E env OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_PIPELINE=8
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,1,2,3 -t 60 -- $<TARGET_FILE:shmem_test_scan>)
    set_tests_properties(shmem.scan.pipeline-1.fanout-2.0-1-2-3-4-0-1-2-3-4
      shmem.scan.pipeline-4.fanout-2.0-1-2-3-4-0-1-2-3-4 PROPERTIES LABELS "tier2" TIMEOUT 120)
    set_tests_properties(shmem.scan.pipeline-8.0-1-2-3 PROPERTIES LABELS "tier2" TIMEOUT 90)
    osoe_internode_label(shmem.scan.force-remote.0-0-1-1 shmem.scan.fanout-2.0-1-2-3-4-0-1-2-3-4
      shmem.scan.pipeline-1.fanout-2.0-1-2-3-4-0-1-2-3-4
      shmem.scan.pipeline-4.fanout-2.0-1-2-3-4-0-1-2-3-4 shmem.scan.pipeline-8.0-1-2-3)
  endif()
endif()

# Contexts (spec §9.5; design §4.6, §4.11; phase 1 step 8 slice 1;
# test-context.c): on one node, across nodes on a block and an interleaved
# map, and under force-remote; the multi-node maps with the landing records
# small, so that a second context's get finds them taken by the first's.
# Then each misuse ending the job by name, and OSOE_MAX_CONTEXTS disagreeing
# between a node's PEs, or outside its range, refused.
osoe_tier2_test(context)
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    osoe_tier2_map_test(context 0,0,1,1 ${osoe_get_nbi_small})
    osoe_tier2_map_test(context 0,1,2,0,1,0 ${osoe_get_nbi_small})
    add_test(NAME shmem.context.force-remote.0-0-1-1
      COMMAND ${CMAKE_COMMAND} -E env OSOE_FORCE_REMOTE=1 ${osoe_get_nbi_small}
              $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 60 -- $<TARGET_FILE:shmem_test_context>)
    set_tests_properties(shmem.context.force-remote.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 90)
    osoe_internode_label(shmem.context.force-remote.0-0-1-1)
  endif()
endif()
foreach(pair
    "destroyed:shmem_ctx_long_put: context 0x[0-9a-f]+ has been destroyed"
    "foreign:shmem_ctx_long_put: 0x[0-9a-f]+ is not a context this library issued"
    "invalid:shmem_ctx_long_put: SHMEM_CTX_INVALID is not a context this operation may use"
    "outside:shmem_ctx_long_put: PE 2 is not in 0..1 of team 3, the team of the context"
    "default:shmem_ctx_destroy: SHMEM_CTX_DEFAULT was not created by shmem_ctx_create"
    "private-team:shmem_team_destroy: context 0x[0-9a-f]+, created from team 3 with SHMEM_CTX_PRIVATE, is still live"
    "after-team:shmem_ctx_long_put: context 0x[0-9a-f]+ has been destroyed")
  string(REPLACE ":" ";" parts "${pair}")
  list(GET parts 0 which)
  list(GET parts 1 expression)
  add_test(NAME shmem.context.misuse.${which}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_CONTEXT=${which}
            $<TARGET_FILE:osoe-launch> -n 4 -t 30 -- $<TARGET_FILE:shmem_test_context>)
  set_tests_properties(shmem.context.misuse.${which} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "${expression}")
endforeach()
osoe_refusal_test(shmem.max-contexts.local-disagree 2 0
  LAUNCH -n 2 -e 1:OSOE_MAX_CONTEXTS=128
  PATTERNS "osoe: PE 1: OSOE_MAX_CONTEXTS differs between PEs: the local lead has 64, this PE has 128"
           "osoe: PE 0: another local PE failed during rendezvous"
  PROGRAM $<TARGET_FILE:shmem_test_rma>)
foreach(value 0 4097 x)
  add_test(NAME shmem.max-contexts.invalid.${value}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_MAX_CONTEXTS=${value}
            $<TARGET_FILE:osoe-launch> -n 1 -t 30 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.max-contexts.invalid.${value} PROPERTIES LABELS "tier2" TIMEOUT 60
    PASS_REGULAR_EXPRESSION "OSOE_MAX_CONTEXTS='${value}' is not between 1 and 4096")
endforeach()
# And small pools: the exhaustion check scales to them, and a pool of one -
# the default context alone - refuses every creation.
foreach(pool 1 3)
  add_test(NAME shmem.context.pool-${pool}.L2
    COMMAND ${CMAKE_COMMAND} -E env OSOE_MAX_CONTEXTS=${pool}
            $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_context>)
  set_tests_properties(shmem.context.pool-${pool}.L2 PROPERTIES LABELS "tier2" TIMEOUT 90)
endforeach()

# The multiple-element waits (§9.11.2-§9.11.14). It reads the waiter's published
# §4.9 watch word to check the unmasked-span refinement, which is the one place
# a test needs the encoding itself rather than a name in the metric table.
# The profiling interface (§10): the test program interposes three routines,
# which only works because the library defines them under pshmem_ names with
# shmem_ as a weak alias.
osoe_tier2_test(surface)

# The one-node agent's wake: the fastest of five finalizes well under the
# agent's 100 ms sweep, which a lost wake makes every finalize wait out (the
# v0.2 step 1 review, correctness K3; test-agent-wake.c).
add_executable(shmem_test_agent-wake test-agent-wake.c)
target_link_libraries(shmem_test_agent-wake PRIVATE osoe)
osoe_warnings(shmem_test_agent-wake)
add_test(NAME shmem.agent-wake.L4
  COMMAND osoe-launch -n 4 -t 60 -- $<TARGET_FILE:shmem_test_agent-wake>)
set_tests_properties(shmem.agent-wake.L4 PROPERTIES LABELS "tier2" TIMEOUT 90)

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
# And with the link set by hand (v0.2 step 2): osoe-stat shows it.
add_test(NAME shmem.stat.link
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>
          OSOE_LINK_HOP_NS=86000 OSOE_LINK_MESSAGE_NS=7000 OSOE_LINK_BYTES_PER_SECOND=116000000
          $<TARGET_FILE:osoe-launch> -n 2 -t 60 -- $<TARGET_FILE:shmem_test_stat>)
set_tests_properties(shmem.stat.link PROPERTIES LABELS "tier2" TIMEOUT 90)

# A job that dies, dies everywhere (§3.15; phase 1 step 4): PE 0 calls
# shmem_global_exit, or ends the job with a fatal, while every other PE computes
# outside the library, where only its agent can end it. For a global exit the
# caller must leave with the status and every other rank with 0 (abort.h), for
# a fatal every rank with the failure; the launcher's own status must be the
# job's, as srun's is; no rank may be killed by the launcher or outlast its
# computing, and for a global exit every rank's unflushed stdout must have been
# written (§9.1.6). See global-exit.sh. Across
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
# §9.1.6's details on one node, in both builds - a non-lead caller, a fatal, an
# exit straight after init, start_pes's finalize, a program that returned, the
# caller's handlers, a status of 0, two callers at once - which ran over two
# nodes alone, so that the single-node build ran none of them (the v0.2 step 1
# review, conformance C4 and correctness K1). The cases below.
foreach(case "nonlead;7;7;1;compute" "fatal.nonlead;fatal;1;1;compute"
             "immediate;7;7;1;immediate" "start-pes;7;7;0;start-pes"
             "returned;7;7;0;returned" "handlers;7;7;1;handlers"
             "zero;0;0;1;compute" "two-callers;7;7;0;two-callers")
  list(GET case 0 label)
  list(GET case 1 how)
  list(GET case 2 status)
  list(GET case 3 caller)
  list(GET case 4 mode)
  add_test(NAME shmem.global-exit.${label}.one-node
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 - ${how} ${status} ${caller} ${mode})
  set_tests_properties(shmem.global-exit.${label}.one-node
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endforeach()
# And on one node the grace's own status and an end while PEs stream to a
# node-mate - not under tsan, for the reasons the two-node cases below give.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  foreach(case "held-stdout;1;held-stdout" "caller-held-stdout;1;caller-held-stdout"
               "stream;0;stream")
    list(GET case 0 label)
    list(GET case 1 caller)
    list(GET case 2 mode)
    add_test(NAME shmem.global-exit.${label}.one-node
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 4 - 7 7 ${caller} ${mode})
    set_tests_properties(shmem.global-exit.${label}.one-node
      PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  endforeach()
endif()
if(OSOE_INTERNODE)
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
    osoe_internode_label(shmem.global-exit.${label} shmem.global-exit.fatal.${label})
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
    osoe_internode_label(shmem.global-exit.${label}.0-0-1-1)
  endforeach()
  # The grace's own status (abort.c), which nothing else reaches: every other PE
  # holding stdout's lock, so its flush cannot end it and the grace does, with 0;
  # and the caller holding it, so exit's flush waits and the grace ends the caller
  # with its status. About 10 s each (OSOE_ABORT_GRACE_SECONDS). Not under tsan,
  # whose _exit takes stdout's lock itself: a program reduced to a thread holding
  # the lock and another calling _exit ends in 1 s without it and not at all with
  # it (docs/decisions/2026-10-02-release-fixes.md, Part 2) - the grace cannot
  # end what tsan holds open.
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    foreach(mode held-stdout caller-held-stdout)
      add_test(NAME shmem.global-exit.${mode}.0-0-1-1
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 1 ${mode})
      set_tests_properties(shmem.global-exit.${mode}.0-0-1-1
        PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
      osoe_internode_label(shmem.global-exit.${mode}.0-0-1-1)
    endforeach()
  endif()
  # A global exit with status 0: no rank leaves nonzero, and the launcher exits 0.
  add_test(NAME shmem.global-exit.zero.0-0-1-1
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 0 0 1 compute)
  set_tests_properties(shmem.global-exit.zero.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  # Two callers at once on one node: the declaration that wins carries its status
  # and leaves last; the other caller leaves with 0 (abort.c).
  add_test(NAME shmem.global-exit.two-callers.0-0-1-1
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 0 two-callers)
  set_tests_properties(shmem.global-exit.two-callers.0-0-1-1
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  osoe_internode_label(shmem.global-exit.zero.0-0-1-1 shmem.global-exit.two-callers.0-0-1-1)
endif()
# Under a launcher that ends the job at the first bad exit, as srun -K1 does
# (KILLS=yes: no -k): the PE carrying the status leaves last (abort.c), so its
# exit, the one bad one, finds no rank to kill, and it finishes. The round-three
# review's C2, where the others' exits came first and killed the caller inside
# its handler: on one node, and with a handler of a second on a non-lead
# caller. Then what Part 2's adversarial read found the caller could leave
# before: another node's PEs, still being told, for a non-lead caller on either
# node (2 and 7 runs of 30 under srun before); a second caller, inside its
# handlers; and PEs whose end waits on stdout's lock, which their grace ends
# after 10 s - not under tsan, whose _exit waits on the lock for good (below).
foreach(case "L4;4;-;0;compute" "slow-handler.0-0-1-1;4;0,0,1,1;1;slow-handler"
             "slow-handler.one-node;4;-;1;slow-handler" "nonlead.one-node;4;-;1;compute"
             "two-callers.one-node;4;-;0;two-callers"
             "nonlead.0-0-1-1;4;0,0,1,1;1;compute"
             "nonlead-other-node.0-0-1-1;4;0,0,1,1;3;compute"
             "two-callers.0-0-1-1;4;0,0,1,1;0;two-callers")
  list(GET case 0 label)
  list(GET case 1 count)
  list(GET case 2 map)
  list(GET case 3 caller)
  list(GET case 4 mode)
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.global-exit.kills.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> ${count} ${map} 7 7 ${caller} ${mode})
  set_tests_properties(shmem.global-exit.kills.${label}
    PROPERTIES LABELS "tier2" TIMEOUT 90 ENVIRONMENT "KILLS=yes"
               PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.global-exit.kills.${label})
  endif()
endforeach()
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_test(NAME shmem.global-exit.kills.held-stdout.one-node
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_global-exit> 4 - 7 7 1 held-stdout)
  set_tests_properties(shmem.global-exit.kills.held-stdout.one-node
    PROPERTIES LABELS "tier2" TIMEOUT 90 ENVIRONMENT "KILLS=yes"
               PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
endif()
if(OSOE_INTERNODE)
  if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
    add_test(NAME shmem.global-exit.kills.held-stdout.0-0-1-1
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 1 held-stdout)
    # One PE held, of the other node, a non-lead: with every PE held the leads'
    # own graces lined the ends up, and a test passed with either lead's wait
    # deleted (Part 2's second adversarial read). Here only the waits keep the
    # order - its lead's before its end of stream, the caller's lead's for it.
    add_test(NAME shmem.global-exit.kills.held-other-node.0-0-1-1
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 1 held-other-node)
    # Three nodes, the held PE on one that may hear of the exit from the third
    # first: a lead told so used to end its stream to the caller's node at once,
    # which took it for its node's end, and the launcher killed its PEs - eight
    # runs in forty (the same read). Ranks 1 and 2 are node 2, rank 3 node 1.
    add_test(NAME shmem.global-exit.kills.held-other-node.0-2-2-1
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_global-exit> 4 0,2,2,1 7 7 0 held-other-node)
    set_tests_properties(shmem.global-exit.kills.held-stdout.0-0-1-1
                         shmem.global-exit.kills.held-other-node.0-0-1-1
      PROPERTIES LABELS "tier2" TIMEOUT 90 ENVIRONMENT "KILLS=yes"
                 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    set_tests_properties(shmem.global-exit.kills.held-other-node.0-2-2-1
      PROPERTIES LABELS "tier2" TIMEOUT 90 ENVIRONMENT "KILLS=yes;OSOE_TEST_HELD_RANK=2"
                 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
    osoe_internode_label(shmem.global-exit.kills.held-stdout.0-0-1-1
      shmem.global-exit.kills.held-other-node.0-0-1-1
      shmem.global-exit.kills.held-other-node.0-2-2-1)
  endif()
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
    osoe_internode_label(shmem.global-exit.stream.0-0-1-1
      shmem.global-exit.stream.two-buffers.0-0-1-1)
  endif()
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
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.dead-pe.${label}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dead-pe.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_dead-pe> ${count} ${map} ${dies})
  set_tests_properties(shmem.dead-pe.${label}
    PROPERTIES LABELS "tier2" TIMEOUT 90 PASS_REGULAR_EXPRESSION "DEAD PE OK")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.dead-pe.${label})
  endif()
endforeach()
# And on a kernel without pidfd_open - before Linux 5.3, which the single-node
# build's floor of 4.17 admits - where liveness watches by pid: the death
# found all the same, and the lead saying once that it watches so (the v0.2
# step 1 review, correctness K5; deny-pidfd.c).
add_executable(deny-pidfd deny-pidfd.c)
osoe_warnings(deny-pidfd)
foreach(case "sibling;3" "lead;0")
  list(GET case 0 who)
  list(GET case 1 dies)
  add_test(NAME shmem.dead-pe.no-pidfd.${who}.L4
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dead-pe.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_dead-pe> 4 0,0,0,0 ${dies})
  set_tests_properties(shmem.dead-pe.no-pidfd.${who}.L4 PROPERTIES LABELS "tier2" TIMEOUT 90
    ENVIRONMENT "WRAP=$<TARGET_FILE:deny-pidfd>"
    PASS_REGULAR_EXPRESSION "watching local PEs by pid instead.*DEAD PE OK")
endforeach()
# internode: dead-pe-init.sh launches a job of two nodes
if(OSOE_INTERNODE)
  # A lead that dies inside shmem_init, before any agent runs: its non-lead must
  # find it from init's own sweeps. See dead-pe-init.sh.
  add_test(NAME shmem.dead-pe.lead-in-init
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/dead-pe-init.sh $<TARGET_FILE:osoe-launch>
            $<TARGET_FILE:shmem_test_dead-pe>)
  set_tests_properties(shmem.dead-pe.lead-in-init
    PROPERTIES LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "DEAD LEAD IN INIT OK")
  osoe_internode_label(shmem.dead-pe.lead-in-init)
endif()

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
    osoe_map_nodes("${map}" nodes)
    if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
      continue()
    endif()
    add_test(NAME shmem.death-in-wait.${where}.${label}
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/death-in-wait.sh $<TARGET_FILE:osoe-launch>
              $<TARGET_FILE:shmem_test_death-in-wait> ${count} ${map} ${where} ${routine})
    set_tests_properties(shmem.death-in-wait.${where}.${label} PROPERTIES
      LABELS "tier2" TIMEOUT 60 PASS_REGULAR_EXPRESSION "DEATH IN WAIT OK")
    if(nodes GREATER 1)
      osoe_internode_label(shmem.death-in-wait.${where}.${label})
    endif()
  endforeach()
endforeach()

# The version (cmake/OsoeVersion.cmake): what the library prints at start-up
# is compared with the project's own version, never with a copy of it.
# SHMEM_VERSION=1 has every PE say "osoe <version>, OpenSHMEM 1.6" before the
# rendezvous, so that a job that fails to start still says which library it
# was; SHMEM_INFO=1 has a node's lead begin its text with "osoe <version>".
add_test(NAME shmem.version.start-up
  COMMAND osoe-launch -n 1 -t 60 -e 0:SHMEM_VERSION=1 -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.version.start-up PROPERTIES
  PASS_REGULAR_EXPRESSION "(^|\n)osoe: PE 0: osoe ${OSOE_VERSION_PATTERN}, OpenSHMEM 1[.]6\n")
add_test(NAME shmem.version.info
  COMMAND osoe-launch -n 1 -t 60 -e 0:SHMEM_INFO=1 -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.version.info PROPERTIES
  PASS_REGULAR_EXPRESSION "(^|\n)osoe ${OSOE_VERSION_PATTERN} environment variables\n")
# The deprecated SMA_ spellings of the two, and SHMEM_DEBUG under both of its
# names, which no test ran (the v0.2 step 1 review, C4).
add_test(NAME shmem.version.start-up.sma
  COMMAND osoe-launch -n 1 -t 60 -e 0:SMA_VERSION=1 -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.version.start-up.sma PROPERTIES
  PASS_REGULAR_EXPRESSION "(^|\n)osoe: PE 0: osoe ${OSOE_VERSION_PATTERN}, OpenSHMEM 1[.]6\n")
add_test(NAME shmem.version.info.sma
  COMMAND osoe-launch -n 1 -t 60 -e 0:SMA_INFO=1 -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.version.info.sma PROPERTIES
  PASS_REGULAR_EXPRESSION "(^|\n)osoe ${OSOE_VERSION_PATTERN} environment variables\n")
foreach(variable SHMEM_DEBUG SMA_DEBUG)
  add_test(NAME shmem.debug.${variable}
    COMMAND osoe-launch -n 1 -t 60 -e 0:${variable}=1 -- $<TARGET_FILE:shmem_test_init>)
  set_tests_properties(shmem.debug.${variable} PROPERTIES
    PASS_REGULAR_EXPRESSION "osoe: PE 0 of 1 [(]local 0 of 1[)]: segment /dev/shm/osoe[.]")
endforeach()
set_tests_properties(shmem.version.start-up shmem.version.info shmem.version.start-up.sma
  shmem.version.info.sma shmem.debug.SHMEM_DEBUG shmem.debug.SMA_DEBUG PROPERTIES
  LABELS "tier2" TIMEOUT 90
  FAIL_REGULAR_EXPRESSION "exited with status [1-9]|killed by signal|timeout after|left behind")

# Init must refuse what it cannot run, saying why.
add_test(NAME shmem.refuse.no_launcher
  COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_PROCID --unset=SLURM_JOB_ID
          --unset=SLURM_PROCID --unset=SLURM_STEP_ID $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.no_launcher PROPERTIES
  PASS_REGULAR_EXPRESSION "no launcher environment")
# Inside a Slurm job but not a step of it - a program run straight from a
# batch script - which has a job id and no task of its own (§6.1).
add_test(NAME shmem.refuse.slurm_not_a_step
  COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_PROCID --unset=SLURM_PROCID
          --unset=SLURM_STEP_ID SLURM_JOB_ID=4242 $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.slurm_not_a_step PROPERTIES
  PASS_REGULAR_EXPRESSION "inside Slurm job 4242 but not in a step of it")
# A multi-node job is no longer refused; shmem.multinode.* is what it does
# instead, and shmem.init.* still covers the one-node case.
# Run with -k (refusal.sh): every PE ends by itself - here each refuses the
# size it was given - or the node's PEs would wait for the one that failed.
osoe_refusal_test(shmem.refuse.bad_size 2 0
  LAUNCH -n 2 -e 0:SHMEM_SYMMETRIC_SIZE=12q -e 1:SHMEM_SYMMETRIC_SIZE=12q
  PATTERNS "osoe: PE 0: SHMEM_SYMMETRIC_SIZE='12q' is invalid"
           "osoe: PE 1: SHMEM_SYMMETRIC_SIZE='12q' is invalid"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
# The PEs compare their sizes, and each that finds a difference says so; one
# that is already told another failed ends with that instead.
osoe_refusal_test(shmem.refuse.size_mismatch 3 0
  LAUNCH -n 3 -e 2:SHMEM_SYMMETRIC_SIZE=2m
  PATTERNS "osoe: PE [012]: SHMEM_SYMMETRIC_SIZE differs between PEs"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
# The deprecated SMA_ spelling (spec §8, Annex F) must be honoured the same
# way, down to the same validation (conformance review F3): before F3's fix,
# bootstrap.c read SHMEM_SYMMETRIC_SIZE only and SMA_SYMMETRIC_SIZE=12q was
# silently ignored, so this job would have run instead of refusing.
osoe_refusal_test(shmem.refuse.bad_size_sma 2 0
  LAUNCH -n 2 -e 0:SMA_SYMMETRIC_SIZE=12q -e 1:SMA_SYMMETRIC_SIZE=12q
  PATTERNS "osoe: PE 0: SHMEM_SYMMETRIC_SIZE='12q' is invalid"
           "osoe: PE 1: SHMEM_SYMMETRIC_SIZE='12q' is invalid"
  PROGRAM $<TARGET_FILE:shmem_test_init>)
# A zero heap is syntactically valid and never useful (conformance review F9 /
# omissions 11): every shmem_malloc would fail on every PE.
add_test(NAME shmem.refuse.zero_size
  COMMAND osoe-launch -n 1 -t 60 -e 0:SHMEM_SYMMETRIC_SIZE=0
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.zero_size PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE='0' is 0")
# Team handles a program must not use (test-bad-team.c), each undefined by the
# spec and loud here: destroying a predefined team (§9.4.8), and a handle the
# library never issued or this PE has destroyed (phase 1 step 7 slice 2) - the
# destroyed one on two logical nodes, since the destroy before it is a sync
# over a team whose records are on both.
add_executable(shmem_test_bad_team test-bad-team.c)
target_link_libraries(shmem_test_bad_team PRIVATE osoe)
osoe_warnings(shmem_test_bad_team)
add_test(NAME shmem.refuse.destroy_predefined_team
  COMMAND osoe-launch -n 1 -t 60 -- $<TARGET_FILE:shmem_test_bad_team>)
set_tests_properties(shmem.refuse.destroy_predefined_team PROPERTIES
  LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "SHMEM_TEAM_WORLD is a predefined team and cannot be destroyed")
foreach(case_expect
    "shared|1|0|SHMEM_TEAM_SHARED is a predefined team and cannot be destroyed"
    "destroyed|2|0,1|shmem_team_n_pes: 0x[0-9a-f]+ is team 3, which this PE has destroyed"
    "destroyed-twice|2|0,0|shmem_team_destroy: 0x[0-9a-f]+ is team 3, which this PE has destroyed"
    "never|1|0|shmem_team_sync: 0x[0-9a-f]+ is not a team this library issued")
  string(REPLACE "|" ";" parts "${case_expect}")
  list(GET parts 0 case)
  list(GET parts 1 count)
  list(GET parts 2 map)
  list(GET parts 3 expect)
  osoe_map_nodes("${map}" nodes)
  if(nodes GREATER 1 AND NOT OSOE_INTERNODE)
    continue()
  endif()
  add_test(NAME shmem.refuse.team.${case}
    COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_BAD_TEAM=${case}
            $<TARGET_FILE:osoe-launch> -n ${count} -m ${map} -t 60 --
            $<TARGET_FILE:shmem_test_bad_team>)
  set_tests_properties(shmem.refuse.team.${case} PROPERTIES LABELS "tier2" TIMEOUT 90
    PASS_REGULAR_EXPRESSION "${expect}")
  if(nodes GREATER 1)
    osoe_internode_label(shmem.refuse.team.${case})
  endif()
endforeach()
# And the destroyed handle on one node, which the single-node build runs (the
# v0.2 step 1 review, correctness K1).
add_test(NAME shmem.refuse.team.destroyed.one-node
  COMMAND ${CMAKE_COMMAND} -E env OSOE_TEST_BAD_TEAM=destroyed
          $<TARGET_FILE:osoe-launch> -n 2 -m 0,0 -t 60 -- $<TARGET_FILE:shmem_test_bad_team>)
set_tests_properties(shmem.refuse.team.destroyed.one-node PROPERTIES LABELS "tier2" TIMEOUT 90
  PASS_REGULAR_EXPRESSION "shmem_team_n_pes: 0x[0-9a-f]+ is team 3, which this PE has destroyed")

add_executable(shmem_test_bad_put test-bad-put.c)
target_link_libraries(shmem_test_bad_put PRIVATE osoe)
osoe_warnings(shmem_test_bad_put)
add_test(NAME shmem.refuse.nonsymmetric_put
  COMMAND osoe-launch -n 1 -t 60 -- $<TARGET_FILE:shmem_test_bad_put>)
set_tests_properties(shmem.refuse.nonsymmetric_put PROPERTIES
  PASS_REGULAR_EXPRESSION
  "shmem_putmem: .* is not within the symmetric heap, and this program was not linked with oshcc")
set_tests_properties(shmem.refuse.no_launcher shmem.refuse.slurm_not_a_step
  shmem.refuse.zero_size shmem.refuse.nonsymmetric_put PROPERTIES
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
    "ptr|shmem_ptr: called before shmem_init"
    # The active-set routines check initialization before the active set,
    # whose "whole job" is of 0 PEs before init.
    "barrier|shmem_barrier: called before shmem_init"
    "sum_to_all|shmem_long_sum_to_all: called before shmem_init")
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
# Every tier-3 test is a job of several logical nodes, each in a network
# namespace of its own: the single-node build, which refuses such a job, has
# none to register, and says so rather than register nothing in silence.
if(OSOE_TIER3 AND OSOE_SINGLE_NODE)
  message(FATAL_ERROR "OSOE_TIER3=ON registers jobs of several nodes, which the single-node "
                      "build (OSOE_SINGLE_NODE=ON) refuses: tier 3 is the full build's")
endif()
if(OSOE_TIER3 AND CMAKE_C_FLAGS MATCHES "sanitize=thread")
  message(FATAL_ERROR "OSOE_TIER3: the multi-node data-path tests do not run under "
                      "ThreadSanitizer (shmem.internode's reason); use the tier3 preset")
endif()
# internode: every tier-3 test runs each node in a network namespace of its own
if(OSOE_INTERNODE)
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
      osoe_internode_label(tier3.${shape}.${name})
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
      osoe_tier3_test(get-nbi.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_get-nbi
                      ENV ${osoe_tier3_wire} OSOE_GET_LANDING_BYTES=4096
                          OSOE_AM_CREDITS=2)
      # Contexts' gets sharing the landing zone, and a team context's traffic,
      # over the shaped link (phase 1 step 8 slice 1).
      osoe_tier3_test(scan.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_scan
                      ENV ${osoe_tier3_wire})
      osoe_tier3_test(context.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_context
                      ENV ${osoe_tier3_wire} OSOE_GET_LANDING_BYTES=4096
                          OSOE_AM_CREDITS=2)
      # Created teams spanning nodes over the shaped link (the team redesign's step
      # D): the team split program's forty teams on the interleaved map, where the
      # odd ranks' local lead on node 0 is local id 1 and on node 1 local id 0, so
      # a message names its team by a lead that is not the sender's own, and a
      # created team's block seldom its number, each block given back as the
      # program destroys its team and taken again at a new generation - barriers,
      # reductions and the rest on each, through the delay and the loss. Its reuse
      # mode makes two teams in sequence spanning the nodes, the second taking the
      # first's block under another name, which the progress engine follows; and
      # two threads of each PE split two parents at once, one a node.
      osoe_tier3_test(team-split.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_team-split
                      ENV SHMEM_SYMMETRIC_SIZE=16m)
      osoe_tier3_test(team-split.reuse.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_team-split
                      ENV OSOE_TEST_SPLIT=reuse SHMEM_SYMMETRIC_SIZE=16m)
      osoe_tier3_test(team-threads.0-1 ${shape} 0,1 shmem_test_team-threads)
      # The leads' tree of the reductions and broadcast, over the shaped link.
      osoe_tier3_test(reduce.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_reduce)
      osoe_tier3_test(broadcast.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_broadcast)
      osoe_tier3_test(active-set.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_active-set)
      osoe_tier3_test(collect.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_collect)
      osoe_tier3_test(alltoall.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_alltoall)
      osoe_tier3_test(statics.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_statics)
      osoe_tier3_test(lock.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_lock)
      # The deadlock detector over the shaped link (§3.15): a deadlock across
      # three nodes found through the delay and the loss, and a PE computing
      # while the rest wait - which must not be taken for one - through the same.
      osoe_tier3_test(deadlock.mismatch.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_deadlock
                      ENV OSOE_DEADLOCK_SECONDS=1 ARGS mismatch)
      set_tests_properties(tier3.${shape}.deadlock.mismatch.0-1-2-0-1-0 PROPERTIES
        PASS_REGULAR_EXPRESSION "every PE of the job waits in the library"
        FAIL_REGULAR_EXPRESSION "DEADLOCK TEST REACHED|a wait it did not record|timeout after")
      osoe_tier3_test(deadlock.computing.0-1-2-0-1-0 ${shape} 0,1,2,0,1,0 shmem_test_deadlock
                      ENV OSOE_DEADLOCK_SECONDS=1 ARGS computing)
      set_tests_properties(tier3.${shape}.deadlock.computing.0-1-2-0-1-0 PROPERTIES
        PASS_REGULAR_EXPRESSION "DEADLOCK TEST OK: computing"
        FAIL_REGULAR_EXPRESSION "osoe: deadlock|CHECK[(]")
      if(shape STREQUAL "pi")
        osoe_tier3_test(reduce.0-1-2-3-4.k2 pi 0,1,2,3,4 shmem_test_reduce
                        ENV OSOE_COLLECTIVE_FANOUT=2)
      endif()
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
      # The pins are checked only when asked (shmem.floor's comment above), and sudo
      # does not pass on ctest's environment, so the job's is given them here.
      set(osoe_tier3_floor_environment)
      if(OSOE_TEST_PINS)
        set(osoe_tier3_floor_environment ENV OSOE_TEST_FLOOR_PINS=1)
      endif()
      osoe_tier3_test(floor ${shape} 0,1 shmem_test_floor ${osoe_tier3_floor_environment}
                      ARGS 64)
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
        osoe_internode_label(tier3.${shape}.global-exit.${label})
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
      osoe_internode_label(tier3.${shape}.global-exit.immediate.0-0-1-2-3-4-5-6-7)
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
        osoe_internode_label(tier3.${shape}.global-exit.${label}.0-0-1-1)
      endforeach()
      string(REPLACE ";" " " wrap "${osoe_tier3};${shape};2;OSOE_WIRE_BUFFERS=2;--")
      add_test(NAME tier3.${shape}.global-exit.stream.two-buffers.0-0-1-1
        COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
                sh ${CMAKE_CURRENT_SOURCE_DIR}/global-exit.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_global-exit> 4 0,0,1,1 7 7 0 stream)
      set_tests_properties(tier3.${shape}.global-exit.stream.two-buffers.0-0-1-1
        PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "GLOBAL EXIT OK")
      osoe_internode_label(tier3.${shape}.global-exit.stream.two-buffers.0-0-1-1)
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
        osoe_internode_label(tier3.${shape}.vanished-node.${label})
      endforeach()
      # The same with data in flight toward the node when it vanishes, which
      # keepalive does not probe: found by the kernel's silence instead, within
      # keepalive's budget (progress.c, osoe_progress_check_silent).
      string(REPLACE ";" " " wrap
             "${osoe_tier3};${shape};2;TIER3_CUT=1;OSOE_KEEPALIVE=1,1,3;OSOE_TEST_MODE=inflight;--")
      add_test(NAME tier3.${shape}.vanished-node-inflight.0-1
        COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
                sh ${CMAKE_CURRENT_SOURCE_DIR}/vanished-node.sh $<TARGET_FILE:osoe-launch>
                $<TARGET_FILE:shmem_test_vanished-node> 2 0,1)
      set_tests_properties(tier3.${shape}.vanished-node-inflight.0-1
        PROPERTIES LABELS "tier3" TIMEOUT 120 PASS_REGULAR_EXPRESSION "VANISHED NODE OK")
      osoe_internode_label(tier3.${shape}.vanished-node-inflight.0-1)
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
      osoe_internode_label(tier3.${shape}.differential)
    endforeach()

    # Keepalive and the silent-peer check at their defaults where the nodes have
    # addresses of their own - a cluster's default, which a job on loopback
    # addresses turns off (§3.15, §8.1; test-keepalive.c). OSOE_KEEPALIVE is
    # unset, since sudo resets the environment: both leads must find keepalive
    # on, and the silent-peer check saying it is off fails the test. One shape
    # is enough, since the choice is made from the addresses alone.
    osoe_tier3_test(keepalive.default-on.0-1 pi 0,1 shmem_test_keepalive
                    ENV SHMEM_DEBUG=1 ARGS on)
    set_tests_properties(tier3.pi.keepalive.default-on.0-1 PROPERTIES
      PASS_REGULAR_EXPRESSION "KEEPALIVE ON OK"
      FAIL_REGULAR_EXPRESSION "makes no silent-peer check")

    # N1 (the round-three review): a lead that cannot reach a peer says so, where it
    # once retried in silence. Node 1's host list puts node 0 at an address no
    # namespace has, as a typo in a host list would: its connects find no route, each
    # after ARP has given up, and it retries - nothing gives up (§3.15) - and its
    # notice names node 0, the host list's entry, the address dialled and the error;
    # that is the dial's, kept, where it was thrown away. It comes at the first
    # dial to end past the 2 s the test hook asks, since the loop sweeps between
    # dials; when it swept only between rounds of node 0's 32 candidate ports, it
    # came some 25 s of ARP's later, which the launcher's 20 s does not allow.
    # Node 0, which nobody calls,
    # names node 1 and the port it waits on. Both are left to the launcher's
    # timeout. See refusal.sh and tools/launch/tier3.sh.
    string(REPLACE ";" " " wrap "${osoe_tier3};pi;2;OSOE_TEST_NOTICE_SECONDS=2;--")
    add_test(NAME tier3.pi.start-up.wrong-address
      COMMAND ${CMAKE_COMMAND} -E env "WRAP=${wrap}"
              sh ${CMAKE_CURRENT_SOURCE_DIR}/refusal.sh $<TARGET_FILE:osoe-launch> 2 2
              "osoe_notice: shmem_init on node 1 has waited [0-9]+ s for node 0 [(]10[.]99[.]0[.]77, at 10[.]99[.]0[.]77[)] to answer on its candidate ports in OSOE_PORT_RANGE=[0-9]+-[0-9]+: (No route to host|Host is down|Network is unreachable|the connection timed out)"
              "osoe_notice: shmem_init on node 0 has waited [0-9]+ s for node 1, its child in the rendezvous tree, to call it on port [0-9]+"
              -- -n 2 -m 0,1 -t 20 -e 1:OSOE_STEP_NODELIST=10.99.0.77,10.99.0.2
              -- $<TARGET_FILE:shmem_test_job>)
    set_tests_properties(tier3.pi.start-up.wrong-address PROPERTIES LABELS "tier3" TIMEOUT 120
      PASS_REGULAR_EXPRESSION "REFUSAL OK")
    osoe_internode_label(tier3.pi.start-up.wrong-address)
  endif()
endif()
