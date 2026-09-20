# base: internal building blocks linked into libosoe. Never installed.
add_library(osoe_base STATIC
  counters.h
  deadline.c
  environment.c
  node-map.c
  epoch.c
  futex.c
  order.c
  size.c
  sweep.c)
target_include_directories(osoe_base PUBLIC ${PROJECT_SOURCE_DIR})
set_target_properties(osoe_base PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)
osoe_warnings(osoe_base)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
