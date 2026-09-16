cmake_minimum_required(VERSION 3.25)

project(osoe
  VERSION 0.0.0
  DESCRIPTION "OpenSHMEM 1.6 over Ethernet with io_uring"
  LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "osoe requires Linux (io_uring and futex2). On macOS, build inside the "
    "development VM: see dev/lima/create.sh and docs/design, section 8.5.")
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported. Delete the CMakeCache.txt and "
    "CMakeFiles/ this attempt created, then use a preset, which builds under "
    "build/<preset>:  cmake --workflow --preset debug")
endif()

option(BUILD_SHARED_LIBS "Build libosoe as a shared library" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CTest)
include(OsoeWarnings)

# liburing comes from the distribution; older releases are unsupported rather
# than accommodated with a bundled copy (design notes, section 1).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING IMPORTED_TARGET "liburing>=2.6")
if(NOT LIBURING_FOUND)
  message(FATAL_ERROR
    "liburing 2.6 or newer is required (io_uring futex operations). "
    "On Debian 13 or Raspberry Pi OS Trixie: apt install liburing-dev. "
    "Older OS releases are not supported.")
endif()

add_subdirectory(shmem)
add_subdirectory(tools)
