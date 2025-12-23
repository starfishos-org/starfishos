#include "polling_tp.h"
#include "polling_resp.h"

void thread_pool_init(thread_pool_t *pool)
{
    pool->head = 0;
    pool->tail = 0;
    pool->stop = 0;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_create(&pool->threads[i], NULL, polling_worker_thread, pool);
    }
}

void thread_pool_destroy(thread_pool_t *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
}

void thread_pool_add_task(thread_pool_t *pool, struct shm_msg *msg)
{
    pthread_mutex_lock(&pool->lock);
    pool->queue[pool->tail % MAX_QUEUE] = msg;
    pool->tail++;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

void *polling_worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;

    while (1) {
        pthread_mutex_lock(&pool->lock);
        while (pool->head == pool->tail && !pool->stop) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        if (pool->stop && pool->head == pool->tail) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        struct shm_msg *msg = pool->queue[pool->head % MAX_QUEUE];
        pool->head++;
        pthread_mutex_unlock(&pool->lock);

        atomic_store_explicit(
                &msg->state, MSG_RESP_WRITING, memory_order_relaxed);
        handle_polling_fs_request(msg);
        atomic_store_explicit(
                &msg->state, MSG_RESP_READY, memory_order_release);
    }

    return NULL;
}
