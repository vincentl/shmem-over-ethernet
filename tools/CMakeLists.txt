# SPDX-License-Identifier: LGPL-3.0-or-later
# Development tools. Built with the project, and not installed - except
# osoe-stat, which has to exist on a machine where a job hung (§3.14) rather
# than in someone's build tree, the compiler wrapper, oshcc, which programs are
# built with there, osoe-launch, which runs a job where there is no Slurm, as
# on a desktop (v0.2 step 1), and osoe-pacer, the stress tester, whose smoke
# test checks an install (the README's "Installing").
add_subdirectory(modelcheck)
add_subdirectory(probe)
# io_uring's floor (§8.4): the full build's alone, since the single-node build
# has no io_uring to measure (OSOE_SINGLE_NODE).
if(NOT OSOE_SINGLE_NODE)
  add_subdirectory(echo-uring)
endif()
add_subdirectory(launch)
add_subdirectory(stat)
add_subdirectory(bench)
add_subdirectory(pacer)
add_subdirectory(wrapper)
