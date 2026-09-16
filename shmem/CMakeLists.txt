add_library(osoe src/info.c)
target_include_directories(osoe
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    src)
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
