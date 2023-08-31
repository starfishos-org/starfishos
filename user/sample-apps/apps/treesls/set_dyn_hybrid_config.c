#include <stdio.h>
#include <stdlib.h>
#include <chcore/syscall.h>

int main(int argc, char *argv[]) {
	u64 hotness = 64, access_interval = 64;

    if (argc > 2) {
            hotness = atoi(argv[1]);
            access_interval = atoi(argv[2]);
    }
	usys_set_dyn_args(hotness, access_interval);

	return 0;
}
