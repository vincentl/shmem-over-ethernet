find_package(Threads REQUIRED)

add_library(osoe
  src/agent.c
  src/boot.c
  src/fatal.c
  src/info.c
  src/init.c
  src/node.c)
target_include_directories(osoe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)
target_link_libraries(osoe PRIVATE osoe_base Threads::Threads)
set_target_properties(osoe PROPERTIES
  C_VISIBILITY_PRESET hidden
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
osoe_warnings(osoe)

install(TARGETS osoe)
install(FILES include/shmem.h TYPE INCLUDE)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
