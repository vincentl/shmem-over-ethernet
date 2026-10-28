# SPDX-License-Identifier: LGPL-3.0-or-later
find_package(Threads REQUIRED)

# The backend behind transport.h and internode.h (§4.10), bound at build time:
# no table of function pointers, one implementation per build. AM is the only
# one; the option exists so that a second is a new list here and not an edit
# to the library's sources (second review, roce-seam F9).
set(OSOE_BACKEND "am" CACHE STRING "The transport and internode backend (am)")
set_property(CACHE OSOE_BACKEND PROPERTY STRINGS am)
if(OSOE_BACKEND STREQUAL "am")
  # The part of the backend every job uses: the dispatch on locality, the
  # contexts, and a one-node job's fence and wait (transport-am.c); and the
  # generic internode phase, which AM hands the data collectives to and whose
  # team-number calls every job makes - any backend may compile it (it is on
  # the boundary, cmake/CheckBoundary.cmake).
  set(osoe_backend_sources
    src/internode-generic.c
    src/transport-am.c)
  # The internode backend (internode-backend.h): every file only a job of more
  # than one node uses - the far half, the engine, the wires and the ring, the
  # connection area, the replies and the messages, the rendezvous of the node
  # leads, and internode.h over AM.
  set(osoe_internode_sources
    src/am.c
    src/connection.c
    src/connection-area.c
    src/data-wires.c
    src/internode-am.c
    src/listener.c
    src/mesh-schedule.c
    src/progress.c
    src/progress-host.c
    src/rendezvous.c
    src/reply.c
    src/ring.c
    src/transport-internode.c
    src/wire.c)
  # The single-node build compiles none of them: the refusing backend stands
  # in their place, refusing a job of more than one node at shmem_init by name
  # (OSOE_SINGLE_NODE, the top-level CMakeLists.txt).
  if(OSOE_SINGLE_NODE)
    set(osoe_internode_sources src/transport-single-node.c)
  endif()
else()
  message(FATAL_ERROR "OSOE_BACKEND=${OSOE_BACKEND}: the only backend is am")
endif()

add_library(osoe
  src/abort.c
  src/accessible.c
  src/agent.c
  src/allocator.c
  src/alltoall.c
  src/am-configuration.c
  src/amo.c
  src/barrier.c
  src/blocked.c
  src/bootstrap.c
  src/collective-circulant.c
  src/collective-shape.c
  src/collective-tree.c
  src/deadlock.c
  src/deprecated.c
  src/fatal.c
  src/info.c
  src/init.c
  src/job.c
  src/link-measure.c
  src/liveness.c
  src/lock.c
  src/malloc.c
  src/metrics.c
  src/node.c
  src/perturb.c
  src/profile.c
  src/quiet.c
  src/rank-map.c
  src/rma.c
  src/rma-typed.c
  src/scans.c
  src/active-set.c
  src/collectives.c
  src/combine.c
  src/context.c
  src/reductions.c
  src/broadcast.c
  src/collect.c
  src/session.c
  src/shm-object.c
  src/signal.c
  src/symmetric.c
  src/team.c
  src/transport-local.c
  src/unknown-settings.c
  src/region-wait.c
  src/wait.c
  src/watch.c
  src/sweep.c
  src/wire-settings.c
  ${osoe_backend_sources}
  ${osoe_internode_sources})
target_include_directories(osoe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)
# liburing is the one library §1's budget allows beside libc (invariant 15);
# the AM backend's ring is its first user (phase 1 step 3), and the
# single-node build has none.
target_link_libraries(osoe PRIVATE osoe_base Threads::Threads)
if(NOT OSOE_SINGLE_NODE)
  target_link_libraries(osoe PRIVATE PkgConfig::LIBURING)
endif()

# The shared library's file is libosoe.so.<version> and its soname
# libosoe.so.<major>, from the project's one version (cmake/OsoeVersion.cmake);
# the start-up line and SHMEM_INFO's text print it too (init.c).
set_target_properties(osoe PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
osoe_warnings(osoe)

install(TARGETS osoe)
install(FILES include/shmem.h include/pshmem.h include/shmemx.h TYPE INCLUDE)
# The historical header location (Annex F.2.1): <mpp/shmem.h> and
# <mpp/shmemx.h> must both resolve (conformance review F8).
install(FILES include/mpp/shmem.h include/mpp/shmemx.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mpp)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
