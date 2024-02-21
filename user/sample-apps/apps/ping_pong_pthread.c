#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <string.h>

#include <chcore/syscall.h>
#include <chcore/launcher.h>

#define LEADER   0
#define FOLLOWER 1
#define TRUE     1
#define FALSE    0

const int sleep_every_step = TRUE;

struct shared_data {
        int turn;
        int counter;
};

volatile struct shared_data *region;

struct passed_args {
        int myturn;
        u64 affinity;
};

void *count(void *arg)
{
        struct passed_args *pargs = (struct passed_args *)arg;
        int myturn = pargs->myturn;
        u64 affinity = pargs->affinity;

        /* schedule FOLLOWER to another machine */
        if (affinity != -1) {
                usys_set_affinity(-2, affinity);
                usys_yield();
        }
        printf("%s's ready to ping pong (aff=%lu)\n",
                (myturn == LEADER ? "LEADER": "FOLLOWER"),
                affinity);
        
        for (;;) {
                /* wait for my turn */
                while (region->turn != myturn)
                        ;

                /* output and add 1 */
                printf("%d: [%d]\n", myturn, region->counter++);

                /* give it to the other process */
                region->turn ^= 1;

                if (sleep_every_step) {
                        sleep(1);
                }
        }
        return NULL;
}

#define eq(s1, s2) (!strcmp(s1, s2))

int main(int argc, char *argv[])
{
        // pthread_attr_t attr;
        pthread_t tid;
        int i;
        u64 follower_aff = -1;
        int lastarg;

        for (i = 1; i < argc; i++) {
                lastarg = (i == (argc-1));

                if (eq(argv[i],"-a")) {
                        if (lastarg) goto invalid;
                        follower_aff = atoi(argv[++i]);
                } else if (eq(argv[i],"-help")) {
                        goto usage;
                } else {
                        goto invalid;
                }
        }
        
        /* create shared memory */
        region = (volatile struct shared_data *)malloc(
                sizeof(struct shared_data));
        printf("shared mem created\n");
        assert(region != NULL);

        /* initialize the shared structure */
        region->turn = 0; // leader
        region->counter = 1;

        struct passed_args *args1 = malloc(sizeof(struct passed_args));
        args1->myturn = FOLLOWER;
        args1->affinity = follower_aff;

        /* spawn the follower process */
        pthread_create(&tid, NULL, count, (void *)args1);

        /* start counting */
        struct passed_args *args2 = malloc(sizeof(struct passed_args));
        args2->myturn = LEADER;
        args2->affinity = 0;

        count((void *)args2);
invalid:
        printf("Invalid option \"%s\" or option argument missing\n",argv[i]);
usage:
        printf("Usage: checkpoint.bin [-a <affinity>] [-help]\n"
                " -a <affinity>      set affinity to core <affinity>\n");
        return 0;
}
