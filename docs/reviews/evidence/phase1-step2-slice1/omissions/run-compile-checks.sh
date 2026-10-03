#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Run inside the VM. $1 = private tree root.
T=${1:?tree}; E=$(dirname "$0"); I="-I$T/shmem/include"; L="-L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem"
O=${TMPDIR:-/tmp}/omissions-$$; mkdir -p $O
set -x
g++ -std=c++17 -Wall -Wextra -pedantic $I $E/cxx-include.cpp -o $O/cxx-include $L; echo "rc=$?"
clang++ -std=c++17 -Wall -Wextra -pedantic $I $E/cxx-include.cpp -o $O/cxx-include-clang $L; echo "rc=$?"
g++ -std=c++17 $I -c $E/cxx-generic.cpp -o $O/cxx-generic.o; echo "rc=$?"
gcc -std=c11 -pedantic -Wall -Wextra -Werror $I -c $E/generic-all-types.c -o $O/g.o; echo "rc=$?"
clang -std=c11 -pedantic -Wall -Wextra -Werror $I -c $E/generic-all-types.c -o $O/g2.o; echo "rc=$?"
gcc -std=c99 -pedantic -Wall -Wextra $I -c $E/cxx-include.cpp -x c -o /dev/null 2>/dev/null; true
gcc -std=c11 $I -c $E/generic-spec-examples.c -o $O/s.o; echo "rc=$?"
$T/build/debug/tools/launch/osoe-launch -n 2 -- $O/cxx-include; echo "rc=$?"
