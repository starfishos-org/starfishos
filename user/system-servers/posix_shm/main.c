/*
 * For POSIX-compatability, we need to support interfaces like shmxxx.
 * We implement them in user-level system servers, e.g., this posix_shm.
 */

#include <sys/ipc.h>
#include <bits/errno.h>
#include <chcore/type.h>
#include <chcore/container/list.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <chcore-internal/shmmgr_defs.h>
#include <pthread.h>
#include <malloc.h>
#include <assert.h>

struct shm_record {
	int key;
	size_t size;
	/* type is PMO_SHM */
	u64 shm_cap;

	struct list_head node;
};

struct list_head global_shm_info;
/* Protect the global_shm_info list */
pthread_mutex_t shm_info_lock;

void init_global_shm_namespace(void)
{
	init_list_head(&global_shm_info);
	pthread_mutex_init(&shm_info_lock, NULL);
}

int handle_shmget(int key, size_t size, int flag)
{
	int shm_cap;
	struct shm_record *record;
	int exist;

	exist = 0;

	/* TODO: allow more flags? */
	BUG_ON(!(flag & IPC_CREAT));

	pthread_mutex_lock(&shm_info_lock);

	for_each_in_list(record, struct shm_record, node, &global_shm_info) {
		if (record->key == key) {
			exist = 1;
			break;
		}
	}

	/* Already exist */
	if ((exist == 1) && (flag & IPC_EXCL)) {
		shm_cap = -EEXIST;
		goto out;
	}

	/* Return the exist shm_cap */
	if (exist == 1) {
		// printf("shm already exists\n");
		shm_cap = record->shm_cap;
		goto out;
	}

	size = ROUND_UP(size, PAGE_SIZE);

	/*
	 * Allocate a new shm_cap.
	 * TODO: free the shm_record later.
	 */
	record = (struct shm_record *)malloc(sizeof(*record));
	record->key = key;
	record->size = size;
	/* Allocate a PMO_SHM for the new shm_cap */
	shm_cap = usys_create_pmo(size, PMO_SHM, MALLOC_TYPE_DEFAULT);
	if (shm_cap < 0) {
		goto out;
	}
	record->shm_cap = shm_cap;

	/* Record the shm_cap */
	list_add(&(record->node), &global_shm_info);

	shm_cap = record->shm_cap;
 out:
	pthread_mutex_unlock(&shm_info_lock);
	return shm_cap;
}

void shmmgr_dispatch(ipc_msg_t * ipc_msg, u64 client_badge)
{
	int key, size, flag;
	struct shm_request *record;
	int ret = 0;

	record = (struct shm_request *)ipc_get_msg_data(ipc_msg);
	key = record->key;
	size = record->size;
	flag = record->flag;

	switch (record->req) {
	case SHM_REQ_GET:
		ret = handle_shmget(key, size, flag);
		if (ret > 0) {
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0, ret);
			ipc_return_with_cap(ipc_msg, 0);
		} else {
			printf("SHMGR: SHM_REQ_CTL error value %d\n", ret);
		}
		break;
	case SHM_REQ_CTL: // we don't support ctl now.
		break;
	default:
		printf("SHMMGR: unvalid req: %x\n", record->req);
		break;
	}
	ipc_return(ipc_msg, ret);
}

int main(int argc, char *argv[], char *envp[])
{
	int ret;

	init_global_shm_namespace();

	ret = ipc_register_server(shmmgr_dispatch, register_cb_single);
	printf("[Shm Manager] register server value = %d\n", ret);

	usys_exit(0);
	return 0;
}
