#pragma once

#include <chcore/ipc.h>
#include <chcore/idman.h>

#define TMPFS_INFO_VADDR 0x200000

#define PREFIX "[fsm]"
#define info(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#if 0
#define debug(fmt, ...) \
	printf(PREFIX "<%s:%d>: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define debug(fmt, ...) do { } while (0)
#endif
#define error(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)

#define MAX_FS_NUM 10			/* TODO: remove */
#define MAX_MOUNT_POINT_LEN 255
#define MAX_PATH_LEN 511

