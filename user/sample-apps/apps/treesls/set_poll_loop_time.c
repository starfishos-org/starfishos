#include <chcore/syscall.h>
#include <stdio.h>
#include <unistd.h>
#include <chcore/type.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <chcore/launcher.h>

int main(int argc, char *argv[]) {
    int lastarg;
    int i;
    for (i = 1; i < argc; i++) {
        // printf("i=%d, argv[1]=%s\n", i, argv[i]);
        lastarg = (i == (argc-1));

        if (!strcmp(argv[i],"-n")) {
            if (lastarg) goto invalid;
            int num = atoi(argv[++i]);
            usys_set_excepted_connected_client_num(num);
	    } else if (!strcmp(argv[i],"-help")) {
            goto usage;
        } else {
            goto invalid;
        }
    }
    return 0;
invalid:
    printf("Invalid option \"%s\" or option argument missing\n", argv[i]);
usage:
	printf("Usage: checkpoint.bin [-n <num>] [-help]\n");
	    //    " -i <interval>      checkpoint interval, %%5 means 5ms, 1 means 1s\n"
	    //    " -t <time>          do how many times of checkpoint, default is infinite\n"
        //    " -v                 output a message when each checkpoint is created\n");
        //    " -m                 enable hot pages tracking and pre-memcpy\n");
}