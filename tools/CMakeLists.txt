# Development tools. Built with the project, and not installed - except
# osoe-stat, which is the one that has to exist on a machine where a job hung
# (§3.14) rather than in someone's build tree.
add_subdirectory(modelcheck)
add_subdirectory(probe)
add_subdirectory(launch)
add_subdirectory(stat)
