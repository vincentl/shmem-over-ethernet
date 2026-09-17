find_package(Threads REQUIRED)

foreach(name size env futex epoch)
  add_executable(base_test_${name} test_${name}.c)
  target_link_libraries(base_test_${name} PRIVATE osoe_base Threads::Threads)
  osoe_warnings(base_test_${name})
  add_test(NAME base.${name} COMMAND base_test_${name})
  set_tests_properties(base.${name} PROPERTIES LABELS "tier1" TIMEOUT 60)
endforeach()
