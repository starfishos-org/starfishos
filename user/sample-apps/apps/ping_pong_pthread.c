#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>

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

void *count(void *arg)
{
        int myturn = (int)(long)arg;

        /* schedule FOLLOWER to another machine */
        if (myturn == FOLLOWER) {
                usys_set_affinity(-2, 15);
                usys_yield();
        }
        printf("I'm ready to ping pong\n");
        
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

int main(int argc, char *argv[])
{
        // pthread_attr_t attr;
        pthread_t tid;
        
        /* create shared memory */
        region = (volatile struct shared_data *)malloc(
                sizeof(struct shared_data));
        printf("shared mem created\n");
        assert(region != NULL);

        /* initialize the shared structure */
        region->turn = 0; // leader
        region->counter = 1;

        /* spawn the follower process */
        pthread_create(&tid, NULL, count, (void *)(long)FOLLOWER);

        /* start counting */
        count((void *)(long)LEADER);
        
        return 0;
}
