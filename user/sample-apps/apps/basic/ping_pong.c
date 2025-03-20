#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

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

void count(int myturn)
{
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
}

void counter_leader(char *program)
{
        /* create shared memory */
        int shmid = shmget(-1, sizeof(struct shared_data), IPC_CREAT);
        assert(shmid > 0);
        printf("created shared memory with id = %d\n", shmid);

        /* map shared memory */
        region = (volatile struct shared_data *)shmat(shmid, 0, 0);
        assert((s64)region != -EINVAL);

        /* initialize the shared structure */
        region->turn = 0; // leader
        region->counter = 1;

        /* spawn the follower process */
        char *argv[] = {program, "follower"};
        chcore_new_process(sizeof(argv) / sizeof(*argv), argv, 0);

        /* start counting */
        count(LEADER);
}

void counter_follower()
{
        /* map shared memory */
        int shmid = shmget(-1, sizeof(struct shared_data), IPC_CREAT);
        region = (volatile struct shared_data *)shmat(shmid, 0, 0);
        assert((s64)region != -EINVAL);

        /* start counting */
        count(FOLLOWER);
}

int main(int argc, char *argv[])
{
        switch (argc) {
        case 1:
                counter_leader(argv[0]);
                break;

        case 2:
                counter_follower();
                break;

        default:
                printf("Please launch this program with 0 or 1 arguments!\n");
                return -1;
        }
        return 0;
}
