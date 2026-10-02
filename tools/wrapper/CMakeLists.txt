# SPDX-License-Identifier: LGPL-3.0-or-later
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
set(OSOE_WRAPPER_RELOCATE "")
configure_file(oshcc.in ${CMAKE_CURRENT_BINARY_DIR}/oshcc @ONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
               GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# The linker script that makes a program's statics symmetric (design §4.3)
# goes beside the library, where the installed wrapper names it. The installed
# wrapper finds the tree it belongs to from where it is, since the prefix is
# not known when osoe is configured: `cmake --install --prefix` chooses it at
# install time, and a package's staging directory is not where it lands. Its
# paths are the install directories, relative to that prefix unless one was
# given as an absolute path.
set(OSOE_WRAPPER_INCLUDE "${CMAKE_INSTALL_INCLUDEDIR}")
set(OSOE_WRAPPER_LIBRARY "${CMAKE_INSTALL_LIBDIR}")
set(OSOE_WRAPPER_STATICS_SCRIPT "${CMAKE_INSTALL_LIBDIR}/osoe/osoe-statics.ld")
file(RELATIVE_PATH bin_to_prefix "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_PREFIX}")
if(bin_to_prefix STREQUAL "")
  set(bin_to_prefix ".")
endif()
set(OSOE_WRAPPER_RELOCATE "\
# Installed: the prefix is where this script is, less ${CMAKE_INSTALL_BINDIR}.
prefix=$(CDPATH= cd -- \"$(dirname -- \"$(readlink -f -- \"$0\")\")/${bin_to_prefix}\" && pwd -P)
case $include in /*) ;; *) include=$prefix/$include ;; esac
case $library in /*) ;; *) library=$prefix/$library ;; esac
case $statics in /*) ;; *) statics=$prefix/$statics ;; esac")
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
# shared library's or a relocatable link, the library and neither of the other
# two. --showme:link names the library; -static is refused by name; a query of
# the compiler, --version, answers as the compiler.
set(script ${CMAKE_CURRENT_SOURCE_DIR}/osoe-statics.ld)
add_test(NAME wrapper.show
  COMMAND sh -c "${oshcc} -show -c x.c | grep -q -- '-I${PROJECT_SOURCE_DIR}/shmem/include -fno-pie' && ! ${oshcc} -show -c x.c | grep -q -- -losoe && ${oshcc} -show -o x x.c | grep -q -- '-no-pie -Xlinker -T -Xlinker ${script} .*-losoe' && ! ${oshcc} -show -shared -o x.so x.c | grep -q -- -no-pie && ! ${oshcc} -show -shared -o x.so x.c | grep -q -- '-Xlinker -T' && ${oshcc} -show -shared -o x.so x.c | grep -q -- -losoe && ! ${oshcc} -show -r -o x.o y.o | grep -q -- -no-pie && ${oshcc} --showme:link | grep -q -- -losoe && ! ${oshcc} -static -o x x.c 2>/dev/null && ${oshcc} -static -o x x.c 2>&1 | grep -q 'link dynamically' && ${oshcc} --version > /dev/null")
# A program with static data, built with nothing but the wrapper: ISx's reduce
# into a static total with a static pSync, across two nodes.
add_test(NAME wrapper.statics
  COMMAND sh -c "mkdir -p ${work} && ${oshcc} -o ${work}/statics ${CMAKE_CURRENT_SOURCE_DIR}/statics.c && $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -- ${work}/statics")
# The installed wrapper, installed under a prefix chosen at install time: it
# names that prefix's headers, library and script, and what it builds runs.
set(prefix ${work}/prefix)
add_test(NAME wrapper.installed
  COMMAND sh -c "rm -rf ${prefix} && ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --prefix ${prefix} > /dev/null && p=$(cd ${prefix} && pwd -P) && ${prefix}/bin/oshcc -show -o x x.c | grep -q -- \"-I$p/${CMAKE_INSTALL_INCLUDEDIR} .*-Xlinker $p/${CMAKE_INSTALL_LIBDIR}/osoe/osoe-statics.ld .*-L$p/${CMAKE_INSTALL_LIBDIR} \" && ${prefix}/bin/oshcc -o ${work}/installed ${CMAKE_CURRENT_SOURCE_DIR}/statics.c && $<TARGET_FILE:osoe-launch> -n 4 -m 0,0,1,1 -t 30 -- ${work}/installed")
set_tests_properties(wrapper.hello wrapper.compile-then-link wrapper.show wrapper.statics
  wrapper.installed PROPERTIES LABELS "tier1" TIMEOUT 60)
# An install writes the build tree's manifest: one at a time (the pacer's
# installed test installs too).
set_tests_properties(wrapper.installed PROPERTIES RESOURCE_LOCK osoe-install)

# osoe's version is --showme:version, which says the version CMake knows
# (cmake/OsoeVersion.cmake). --version stays the compiler's, as wrapper.show
# also requires: build systems run it to tell what CC is, and one that read
# "oshcc (osoe) <version>" there would not know it for a C compiler.
osoe_version_test(wrapper.version ${oshcc} oshcc --showme:version)
add_test(NAME wrapper.version-is-the-compilers
  COMMAND sh -c "test \"$(${oshcc} --version)\" = \"$(${CMAKE_C_COMPILER} --version)\"")
set_tests_properties(wrapper.version-is-the-compilers PROPERTIES LABELS "tier1" TIMEOUT 60)
