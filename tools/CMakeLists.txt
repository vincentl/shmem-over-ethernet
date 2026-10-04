# SPDX-License-Identifier: LGPL-3.0-or-later
# Development tools. Built with the project, and not installed - except
# osoe-stat, which has to exist on a machine where a job hung (§3.14) rather
# than in someone's build tree, the compiler wrapper, oshcc, which programs are
# built with there, and osoe-pacer, the stress tester, whose smoke test checks
# an install (the README's "Installing").
add_subdirectory(modelcheck)
add_subdirectory(probe)
add_subdirectory(echo-uring)
add_subdirectory(launch)
add_subdirectory(stat)
add_subdirectory(bench)
add_subdirectory(pacer)
add_subdirectory(wrapper)
