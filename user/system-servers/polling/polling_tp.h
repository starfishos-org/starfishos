#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_THREADS 4
#define MAX_QUEUE   1024

struct shm_msg;

typedef struct thread_pool {
    pthread_t threads[MAX_THREADS];
    struct shm_msg *queue[MAX_QUEUE];
    int head;
    int tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
} thread_pool_t;

void *polling_worker_thread(void *arg);
void thread_pool_init(thread_pool_t *pool);
void thread_pool_destroy(thread_pool_t *pool);
void thread_pool_add_task(thread_pool_t *pool, struct shm_msg *msg);
