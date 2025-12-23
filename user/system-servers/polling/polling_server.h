#pragma once

#include "polling.h"

void init_polling_shm_region(struct polling_shm_region *shm);
void *polling_reader_thread(void *arg);
void create_polling_thread(u32 shm_id, pthread_t *tid);