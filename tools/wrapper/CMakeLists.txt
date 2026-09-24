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
# A library built under a sanitizer needs its runtime in every program that
# loads it, loaded first: the link gets the flags the library's own programs
# are linked with, which are empty outside the sanitizer presets.
set(OSOE_WRAPPER_LINK_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

set(OSOE_WRAPPER_INCLUDE "${PROJECT_SOURCE_DIR}/shmem/include")
set(OSOE_WRAPPER_LIBRARY "${PROJECT_BINARY_DIR}/shmem")
set(OSOE_WRAPPER_STATICS_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/osoe-statics.ld")
configure_file(oshcc.in ${CMAKE_CURRENT_BINARY_DIR}/oshcc @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
               GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# The linker script that makes a program's statics symmetric (design §4.3)
# goes beside the library, where the installed wrapper names it.
set(OSOE_WRAPPER_INCLUDE "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
set(OSOE_WRAPPER_LIBRARY "${CMAKE_INSTALL_FULL_LIBDIR}")
set(OSOE_WRAPPER_STATICS_SCRIPT "${CMAKE_INSTALL_FULL_LIBDIR}/osoe/osoe-statics.ld")
configure_file(oshcc.in ${CMAKE_CURRENT_BINARY_DIR}/installed/oshcc @ONLY)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/installed/oshcc TYPE BIN)
install(FILES osoe-statics.ld DESTINATION ${CMAKE_INSTALL_LIBDIR}/osoe)

# A program built with nothing but the wrapper runs as a job; and a compile-only
# call followed by a link builds the same program, the way make builds one.
set(oshcc ${CMAKE_CURRENT_BINARY_DIR}/oshcc)
set(work ${CMAKE_CURRENT_BINARY_DIR}/test)
add_test(NAME wrapper.hello
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -o ${work}/hello ${CMAKE_CURRENT_SOURCE_DIR}/hello.c && $<TARGET_FILE:osoe-launch> -n 2 -t 30 -- ${work}/hello")
add_test(NAME wrapper.compile-then-link
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -c -o ${work}/hello.o ${CMAKE_CURRENT_SOURCE_DIR}/hello.c && ${oshcc} -o ${work}/hello-linked ${work}/hello.o && $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -- ${work}/hello-linked")
# -show: a compile-only call gets the header path and -fno-pie, and not the
# library; a program's link gets the library, -no-pie and the statics script; a
# shared library's link gets the library and neither of the other two.
set(script ${CMAKE_CURRENT_SOURCE_DIR}/osoe-statics.ld)
add_test(NAME wrapper.show
  COMMAND sh -c "${oshcc} -show -c x.c | grep -q -- '-I${PROJECT_SOURCE_DIR}/shmem/include -fno-pie' && ! ${oshcc} -show -c x.c | grep -q -- -losoe && ${oshcc} -show -o x x.c | grep -q -- '-no-pie -Wl,-T,${script} .*-losoe' && ! ${oshcc} -show -shared -o x.so x.c | grep -q -- -no-pie && ! ${oshcc} -show -shared -o x.so x.c | grep -q -- -T, && ${oshcc} -show -shared -o x.so x.c | grep -q -- -losoe")
# A program with static data, built with nothing but the wrapper: ISx's reduce
# into a static total with a static pSync, across two nodes.
add_test(NAME wrapper.statics
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -o ${work}/statics ${CMAKE_CURRENT_SOURCE_DIR}/statics.c && $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -- ${work}/statics")
set_tests_properties(wrapper.hello wrapper.compile-then-link wrapper.show wrapper.statics
  PROPERTIES LABELS "tier1" TIMEOUT 60)
