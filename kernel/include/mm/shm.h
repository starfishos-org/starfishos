#pragma once

#include <common/types.h>

#define SHM_DATA_SIZE (PAGE_SIZE * 10UL)

void shm_init(void);
int sys_mmap_shm(u32 shm_id, void *addr);