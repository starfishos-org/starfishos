#include "polling.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    pthread_t tid;
    void *shm_addr;
    create_polling_thread(POLLING_FS_SHM_ID, &tid, &shm_addr);
    join_polling_thread(tid, shm_addr);
    printf("Polling thread exited\n");
    return 0;
}