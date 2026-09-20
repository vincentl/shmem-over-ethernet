// The C11 generic names from C++: spec §9.7 gives them to C11 only, so what
// does a C++ program that uses them see?
#include <shmem.h>
int main() {
    shmem_init();
    int *x = static_cast<int *>(shmem_malloc(sizeof(int)));
    shmem_atomic_add(x, 1, 0);
    shmem_finalize();
}
