# SPDX-License-Identifier: LGPL-3.0-or-later
# Warning flags applied to every osoe target.
function(osoe_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
endfunction()
