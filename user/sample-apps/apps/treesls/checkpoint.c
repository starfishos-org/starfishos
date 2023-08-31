#include <chcore/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <chcore/type.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <chcore/launcher.h>

void wrap_sleep(long dur) {
    struct timespec tv = { .tv_sec = dur / 1000000000, .tv_nsec = dur % 1000000000};
    nanosleep(&tv, &tv);
}


int main(int argc, char *argv[]) {
    long interval = 10l * 1000000000;
    int times = -1, i;
    int lastarg;
    int log_level = 0;
    int verbose = 0;
    int shutdown = 0;
    int reset = 1;

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
        } else if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-s")) {
            if (lastarg) goto invalid;
            reset = atoi(argv[++i]);
            shutdown = 1;
        } else if (!strcmp(argv[i],"-l")) {
            if (lastarg) goto invalid;
            log_level = atoi(argv[++i]);
        } else if (!strcmp(argv[i],"-help")) {
            goto usage;
        } else {
            goto invalid;
        }
    }

    for (i = 0; i != times; i++) {
        wrap_sleep(interval);
        if (log_level == 0) {
            usys_whole_ckpt(0,0);
        } else {
            usys_whole_ckpt_for_test("checkpoint_time", 16, log_level);
        }
        if (verbose) {
            printf("Checkpoint created.\n");
        }
    }

    if (shutdown) {
        usys_shutdown(reset);
    }

    printf("Checkpoint finished.\n");
    usys_wait(usys_create_notifc(), 1, NULL);
    return 0;
invalid:
    printf("Invalid option \"%s\" or option argument missing\n",argv[i]);
usage:
    printf("Usage: checkpoint.bin [-i <interval>] [-t <times>] [-a <affinity>] [-l <log level>] [-s] [-v] [-help]\n"
           " -i <interval>      checkpoint interval, %%5 means 5ms, 1 means 1s\n"
           " -t <time>          do how many times of checkpoint, default is infinite\n"
           " -a <affinity>      set affinity to core <affinity>\n"
           " -l <log level>     set log level to <log level>; 0: no log, 1: ckpt time, 3: detail of each part and each object\n"
           " -s <with reset>    shutdown when checkpoint finished, with reset nvm or not, used for restore test\n"
           " -v                 output a message when each checkpoint is created\n");
	return 0;
}
