#include <bits/errno.h>
#include <chcore/type.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <chcore-internal/fs_defs.h>
#include <chcore-internal/fs_debug.h>
#include <fs_wrapper_defs.h>
#include <fs_vnode.h>
#include <fcntl.h>
#include <pthread.h>
#include <malloc.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include "ext4.h"
#include "file_dev.h"
#include "chcore_ext4_server.h"
#include "chcore_ext4_defs.h"

/* aquired by lwext4 */
struct ext4_blockdev *bd;

/* ext4_server should serve REQ_MOUNT at first and store flag here */
bool mounted;

/* concurrency control */
pthread_mutex_t ext4_meta_lock;

/* whether using page cache */
bool using_page_cache;

#ifdef TEST_COUNT_PAGE_CACHE
struct test_count count;
#endif

struct server_entry *server_entrys[MAX_SERVER_ENTRY_NUM];
RBTree *fs_vnode_list;

static inline void init_ext4_server()
{
	mounted = false;
	init_fs_wrapper();
	pthread_mutex_init(&ext4_meta_lock, NULL);
	using_page_cache = true;
}

int main(void)
{
	int ret;

	init_ext4_server();

	ret = ipc_register_server(fs_server_dispatch, DEFAULT_CLIENT_REGISTER_HANDLER);
	printf("[EXT4 Driver] register server value = %d\n", ret);

	while(1) {
		sched_yield();
	}

	return 0;
}