# Warning flags applied to every eoshmem target.
function(eoshmem_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
endfunction()
