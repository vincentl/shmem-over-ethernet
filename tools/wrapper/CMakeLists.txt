# The compiler wrapper, oshcc (design §4.3, §7.1): the name every OpenSHMEM
# build system looks for - PRK's SHMEMCC, bale's CC=oshcc, ISx's and the NAS
# benchmarks' makefiles - so a program builds against osoe without editing its
# build. Two copies from one template: this build's, for the tests and for
# using a build tree in place, and the installed one, which is the only tool
# besides osoe-stat that a machine running jobs needs.
if(BUILD_SHARED_LIBS)
  set(OSOE_WRAPPER_LIBRARIES "-losoe")
else()
  # A static libosoe carries its own objects but not the libraries it uses.
  set(OSOE_WRAPPER_LIBRARIES "-losoe -luring -lpthread")
endif()
set(OSOE_WRAPPER_COMPILER "${CMAKE_C_COMPILER}")

set(OSOE_WRAPPER_INCLUDE "${PROJECT_SOURCE_DIR}/shmem/include")
set(OSOE_WRAPPER_LIBRARY "${PROJECT_BINARY_DIR}/shmem")
configure_file(oshcc.in ${CMAKE_CURRENT_BINARY_DIR}/oshcc @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
               GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(OSOE_WRAPPER_INCLUDE "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
set(OSOE_WRAPPER_LIBRARY "${CMAKE_INSTALL_FULL_LIBDIR}")
configure_file(oshcc.in ${CMAKE_CURRENT_BINARY_DIR}/installed/oshcc @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/installed/oshcc TYPE BIN)

# A program built with nothing but the wrapper runs as a job; and a compile-only
# call followed by a link builds the same program, the way make builds one.
set(oshcc ${CMAKE_CURRENT_BINARY_DIR}/oshcc)
set(work ${CMAKE_CURRENT_BINARY_DIR}/test)
add_test(NAME wrapper.hello
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -o ${work}/hello ${CMAKE_CURRENT_SOURCE_DIR}/hello.c && $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- ${work}/hello")
add_test(NAME wrapper.compile-then-link
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -c -o ${work}/hello.o ${CMAKE_CURRENT_SOURCE_DIR}/hello.c && ${oshcc} -o ${work}/hello-linked ${work}/hello.o && $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -- ${work}/hello-linked")
# -show: a compile-only call gets the header path and not the library; a link
# gets both.
add_test(NAME wrapper.show
  COMMAND sh -c "${oshcc} -show -c x.c | grep -q -- -I${PROJECT_SOURCE_DIR}/shmem/include && ! ${oshcc} -show -c x.c | grep -q -- -losoe && ${oshcc} -show -o x x.c | grep -q -- -losoe")
set_tests_properties(wrapper.hello wrapper.compile-then-link wrapper.show
  PROPERTIES LABELS "tier1" TIMEOUT 60)
