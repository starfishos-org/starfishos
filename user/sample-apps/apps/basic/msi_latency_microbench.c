#include <stdio.h>
#include <stdlib.h>

#include <chcore/syscall.h>

int main(int argc, char **argv)
{
    int samples = argc > 1 ? atoi(argv[1]) : 100;
    int target_machine = argc > 2 ? atoi(argv[2]) : 1;
    int target_cpu = argc > 3 ? atoi(argv[3]) : 4;

    if (samples <= 0 || target_machine < 0 || target_cpu < 0) {
        fprintf(stderr,
                "usage: %s [samples] [target-machine] [target-local-cpu]\n",
                argv[0]);
        return 2;
    }

    if (usys_set_affinity(-1, 0) != 0)
        return 1;
    usys_yield();

    return usys_ivshmem_msi_bench((u64)target_machine,
                                  (u64)target_cpu,
                                  (u64)samples)
                   == 0
           ? 0
           : 1;
}
