#include "polling.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    pthread_t tid;
    void *shm_addr;
    int mid = usys_get_machine_id();
    /* Each machine has its own shared memory region: dsm_meta->shm_data[machine_id] */
    create_polling_thread(mid, &tid, &shm_addr);
    join_polling_thread(tid, shm_addr);
    printf("Polling thread exited\n");
    return 0;
}