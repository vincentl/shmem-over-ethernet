add_executable(shmem_test_info test_info.c)
target_link_libraries(shmem_test_info PRIVATE osoe)
osoe_warnings(shmem_test_info)
add_test(NAME shmem.info COMMAND shmem_test_info)
set_tests_properties(shmem.info PROPERTIES LABELS "tier1")

if(BUILD_SHARED_LIBS)
  add_test(NAME shmem.exports
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:osoe>
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckExports.cmake)
  set_tests_properties(shmem.exports PROPERTIES LABELS "tier1")
endif()
