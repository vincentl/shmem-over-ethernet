# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# The version, here and nowhere else: the line SHMEM_VERSION=1 prints and the
# first line of SHMEM_INFO's text, every tool's --version (oshcc's is
# --showme:version, which leaves --version the compiler's), and the shared
# library's file name and soname all come from this value (cmake/OsoeVersion.cmake
# says how). 0.1.0 was the first release's, and 0.2.0 is the second's.
project(osoe
  VERSION 0.2.0
  DESCRIPTION "OpenSHMEM 1.6 over Ethernet with io_uring"
  LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "osoe requires Linux (futex, and io_uring in the full build). On macOS, build "
    "inside the development VM: see dev/lima/create.sh and docs/design, section 8.5.")
endif()

# The single-node build (docs/decisions/2026-10-05-v0.2.md, items 2 to 4): an
# opt-in for one machine - a desktop, for developing at small PE counts - whose
# library serves jobs of one node only and refuses any other by name at
# shmem_init. It needs no liburing and no io_uring, and waits with the classic
# futex(2) in place of Linux 6.7's futex2 calls, so it runs on Red Hat 9's
# kernel and its rebuilds' and any distribution's since. Off, the full build is
# what it was: Linux 6.7 and liburing 2.6. The two differ by files, chosen
# here - base/futex-classic.c for base/futex-futex2.c, and
# shmem/src/transport-single-node.c, the refusing backend, for the internode
# backend's files (shmem/src/internode-backend.h) - never by a branch at run
# time.
option(OSOE_SINGLE_NODE "Build for jobs of one node only: no liburing, no io_uring, the classic futex" OFF)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported. Delete the CMakeCache.txt and "
    "CMakeFiles/ this attempt created, then use a preset, which builds under "
    "build/<preset>:  cmake --workflow --preset debug")
endif()

option(BUILD_SHARED_LIBS "Build libosoe as a shared library" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# gnu11 by default, which is what the four presets build and every warning
# lives under OsoeWarnings.cmake regardless. The tree also builds warning-free
# as plain -std=c11 (portability review F12): every feature-test macro GNU's
# default exposure was covering for is now declared per file. "if(NOT
# DEFINED ...)" rather than a plain set(), so that a caller who passes
# -DCMAKE_C_EXTENSIONS=OFF (cmake/CheckStrictC11.cmake does, in its own
# scratch configure) is not silently overridden back to ON here.
if(NOT DEFINED CMAKE_C_EXTENSIONS)
  set(CMAKE_C_EXTENSIONS ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CTest)
include(OsoeWarnings)
include(OsoeVersion)
include(OsoeInternode)

# liburing comes from the distribution; older releases are unsupported rather
# than accommodated with a bundled copy (design notes, section 1). The
# single-node build asks for neither it nor pkg-config.
if(OSOE_SINGLE_NODE)
  message(STATUS "osoe ${PROJECT_VERSION}: the single-node build (OSOE_SINGLE_NODE=ON) - "
                 "jobs of one node only, no liburing, no io_uring, the classic futex")
else()
  # pkg-config not REQUIRED: without it, the message below - which names the
  # single-node build - is still the first a user meets (the v0.2 step 1
  # review, portability Q5).
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBURING IMPORTED_TARGET "liburing>=2.6")
  endif()
  if(NOT LIBURING_FOUND)
    set(osoe_without_pkg_config "")
    if(NOT PKG_CONFIG_FOUND)
      set(osoe_without_pkg_config "pkg-config, which finds it, is not installed either. ")
    endif()
    message(FATAL_ERROR
      "liburing 2.6 or newer is required (io_uring futex operations). "
      "${osoe_without_pkg_config}"
      "On Debian 13 or Raspberry Pi OS Trixie: apt install liburing-dev pkg-config. "
      "Older OS releases are not supported by the full build; a machine that runs "
      "jobs of one node only can configure the single-node build, -DOSOE_SINGLE_NODE=ON, "
      "which needs no liburing.")
  endif()
  message(STATUS "osoe ${PROJECT_VERSION}: the full build - liburing ${LIBURING_VERSION}")
endif()

add_subdirectory(base)
add_subdirectory(shmem)
add_subdirectory(tools)

# In a ThreadSanitizer build every test writes the tool's reports to files, and
# a test after them all judges the files (cmake/OsoeThreadSanitizer.cmake; the
# ninth review's C9-1).
if(BUILD_TESTING AND CMAKE_C_FLAGS MATCHES "sanitize=thread")
  include(OsoeThreadSanitizer)
  osoe_thread_sanitizer_gate()
endif()
