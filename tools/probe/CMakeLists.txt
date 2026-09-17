add_executable(probe probe.c)
target_link_libraries(probe PRIVATE PkgConfig::LIBURING)
osoe_warnings(probe)

# Registered as a test so that `ctest` on a machine whose kernel lacks a
# required facility fails immediately, before any library test runs.
add_test(NAME env.probe COMMAND probe)
set_tests_properties(env.probe PROPERTIES LABELS "env" TIMEOUT 60)
