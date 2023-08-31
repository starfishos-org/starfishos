#include <chcore/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <chcore/type.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

void wrap_sleep(long dur) {
    struct timespec tv = { .tv_sec = dur / 1000000000, .tv_nsec = dur % 1000000000};
    nanosleep(&tv, &tv);
}

int main(int argc, char *argv[]) {
    int interval = 10;
    int times = -1, i;
    int lastarg;

    for (i = 1; i < argc; i++) {
        lastarg = (i == (argc-1));

        if (!strcmp(argv[i],"-i")) {
            if (lastarg) goto invalid;
            if (argv[++i][0] == '%') {
                long nano = atoi(argv[i] + 1);
                interval = nano * 1000000l;
            } else {
                interval = atoi(argv[i]) * 1000000000l;
            }
	    } else if (!strcmp(argv[i],"-t")) {
            if (lastarg) goto invalid;
            times = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-a")) {
            if (lastarg) goto invalid;
            usys_set_affinity(-1, atoi(argv[++i]));
            usys_yield();
        } else {
            goto invalid;
        }
    }

    for (i = 0; i != times; i++) {
        wrap_sleep(interval);
        // usys_top();
        u64 size = usys_get_free_mem_size();
        printf("free mem size = %lu MB\n", size / 1024 / 1024);
    }
    return 0;
invalid:
    printf("Invalid option \"%s\" or option argument missing\n",argv[i]);
    return 0;
}
