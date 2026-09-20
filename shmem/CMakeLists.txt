find_package(Threads REQUIRED)

add_library(osoe
  src/accessible.c
  src/agent.c
  src/am.c
  src/allocator.c
  src/amo.c
  src/barrier.c
  src/bootstrap.c
  src/connection.c
  src/connection-area.c
  src/deprecated.c
  src/fatal.c
  src/info.c
  src/init.c
  src/job.c
  src/malloc.c
  src/metrics.c
  src/node.c
  src/profile.c
  src/progress.c
  src/quiet.c
  src/reply.c
  src/rma.c
  src/session.c
  src/signal.c
  src/symmetric.c
  src/team.c
  src/transport-local.c
  src/wait.c
  src/wire.c
  src/watch.c
  src/watchdog.c)
target_include_directories(osoe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)
target_link_libraries(osoe PRIVATE osoe_base Threads::Threads)

# The development watchdog (§3.15) is a development aid, not the release path:
# on in the debug, asan and tsan presets, which all build Debug, and off in
# release. OSOE_WATCHDOG overrides it at run time either way.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(OSOE_WATCHDOG_DEFAULT 1)
else()
  set(OSOE_WATCHDOG_DEFAULT 0)
endif()
target_compile_definitions(osoe PRIVATE OSOE_WATCHDOG_DEFAULT=${OSOE_WATCHDOG_DEFAULT})
set_target_properties(osoe PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
osoe_warnings(osoe)

install(TARGETS osoe)
install(FILES include/shmem.h include/pshmem.h include/shmemx.h TYPE INCLUDE)
# The historical header location (Annex F): <mpp/shmem.h> must resolve.
install(FILES include/mpp/shmem.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mpp)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
