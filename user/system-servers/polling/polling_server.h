#pragma once

#include "polling.h"

void init_polling_shm_region(struct polling_shm_region *shm);
void *polling_reader_thread(void *arg);
int create_polling_threads(u32 shm_id, pthread_t *tids, int num_threads);
