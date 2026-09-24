find_package(Threads REQUIRED)

# The backend behind transport.h and internode.h (§4.10), bound at build time:
# no table of function pointers, one implementation per build. AM is the only
# one; the option exists so that a second is a new list here and not an edit
# to the library's sources (second review, roce-seam F9).
set(OSOE_BACKEND "am" CACHE STRING "The transport and internode backend (am)")
set_property(CACHE OSOE_BACKEND PROPERTY STRINGS am)
if(OSOE_BACKEND STREQUAL "am")
  set(osoe_backend_sources
    src/am.c
    src/connection.c
    src/connection-area.c
    src/internode-am.c
    src/progress.c
    src/progress-host.c
    src/reply.c
    src/ring.c
    src/transport-am.c
    src/wire.c)
else()
  message(FATAL_ERROR "OSOE_BACKEND=${OSOE_BACKEND}: the only backend is am")
endif()

add_library(osoe
  src/abort.c
  src/accessible.c
  src/agent.c
  src/allocator.c
  src/amo.c
  src/barrier.c
  src/bootstrap.c
  src/collective-tree.c
  src/deprecated.c
  src/fatal.c
  src/info.c
  src/init.c
  src/job.c
  src/listener.c
  src/liveness.c
  src/malloc.c
  src/metrics.c
  src/node.c
  src/profile.c
  src/quiet.c
  src/rank-map.c
  src/rendezvous.c
  src/rma.c
  src/rma-typed.c
  src/collectives.c
  src/reductions.c
  src/broadcast.c
  src/session.c
  src/shm-object.c
  src/signal.c
  src/symmetric.c
  src/team.c
  src/transport-local.c
  src/wait.c
  src/watch.c
  src/sweep.c
  ${osoe_backend_sources})
target_include_directories(osoe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)
# liburing is the one library §1's budget allows beside libc (invariant 15);
# the AM backend's ring is its first user (phase 1 step 3).
target_link_libraries(osoe PRIVATE osoe_base Threads::Threads PkgConfig::LIBURING)

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
