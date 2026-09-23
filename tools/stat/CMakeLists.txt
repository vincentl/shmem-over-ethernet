# osoe-stat is the one tool here that is installed (§3.14): it exists to be run
# on a machine where a job has hung, which is not where the build tree is.
add_executable(osoe-stat stat.c segment.c)
target_include_directories(osoe-stat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(osoe-stat PRIVATE rt)
osoe_warnings(osoe-stat)
install(TARGETS osoe-stat)

# The reader against a segment this build did not write: different record
# layouts, a different table order, metrics it has never heard of, and a layout
# version from the future (§3.14). This is the test of "a tool built from a
# different commit reads a job correctly"; running the tool against a job from
# the same tree tests none of it. It runs the tool too, for the watch word's
# layout range, so it is told where the tool is the way shmem.stat is.
add_executable(osoe_segment_test test-segment.c segment.c)
target_include_directories(osoe_segment_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(osoe_segment_test PRIVATE rt)
osoe_warnings(osoe_segment_test)
add_test(NAME stat.segment COMMAND osoe_segment_test)
set_tests_properties(stat.segment PROPERTIES LABELS "tier1" TIMEOUT 30
  ENVIRONMENT "OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>")

# Reading a segment that is not there, and reading one that is not ours, are
# the two failures a user meets first; both must say so rather than crash.
add_test(NAME stat.absent COMMAND osoe-stat --dump /osoe.no.such.segment.0)
set_tests_properties(stat.absent PROPERTIES LABELS "tier1" TIMEOUT 30
  PASS_REGULAR_EXPRESSION "/osoe.no.such.segment.0: no such job"
  WILL_FAIL FALSE)
add_test(NAME stat.help COMMAND osoe-stat --help)
set_tests_properties(stat.help PROPERTIES LABELS "tier1" TIMEOUT 30
  PASS_REGULAR_EXPRESSION "remove named segments [(]OSOE_KEEP_CONTROL_SEGMENT[)] whose PEs")
