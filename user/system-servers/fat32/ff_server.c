#include <bits/errno.h>
#include <chcore/type.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <pthread.h>
#include <malloc.h>
#include <assert.h>
#include <chcore-internal/fs_defs.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <string.h>

#include <fs_wrapper_defs.h>
#include <chcore-internal/fs_debug.h>
#include <fs_vnode.h>
#include "ff.h"
#include "diskio.h"
#include "fat32_defs.h"

char sd[4];
FATFS *fs;
pthread_mutex_t fat_meta_lock;
bool mounted;
bool using_page_cache;

#ifdef TEST_COUNT_PAGE_CACHE
struct test_count count;
#endif

struct server_entry *server_entrys[MAX_SERVER_ENTRY_NUM];
RBTree *fs_vnode_list;

static inline void init_fat32_server()
{
	/* Initialize for sd IPC */
	sd_ipc_init();

	/* Initilaize fs_wrapper */
	init_fs_wrapper();
	using_page_cache = true;

	/* Initialize private datas */
	strcpy(sd, "sd0");
	mounted = false;
	pthread_mutex_init(&fat_meta_lock, NULL);

	/* Just initialize FATFS struct, and fill it when receive FS_REQ_MOUNT */
	fs = (FATFS *)malloc(sizeof(FATFS));
	memset((unsigned char*)fs, 0, sizeof(FATFS));
}

int main()
{
	int ret;

	init_fat32_server();

	ret = ipc_register_server(fs_server_dispatch, DEFAULT_CLIENT_REGISTER_HANDLER);
	printf("[Fat fs] register server value = %d\n", ret);

	while(1) {
		sched_yield();
	}

	return 0;
}
