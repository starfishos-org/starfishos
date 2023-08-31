#include <sys/shm.h>
#include <stdint.h>
#include <bits/errno.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/syscall.h>
#include <chcore/services.h>
#include <chcore-internal/shmmgr_defs.h>
#include <chcore-internal/procmgr_defs.h>
#include <stdio.h>
#include <debug_lock.h>

#if SYSTEMV_SHMMGR == ON
static ipc_struct_t* shmmgr_ipc_struct = NULL;
/* To create connection only once */
static int volatile connect_shmmgr_lock = 0;
#endif

/*
 * Get shmid by key.
 * Return shm_cap as shmid.
 * Only support IPC_CREAT now.
*/
int shmget(key_t key, size_t size, int flag)
{
#if SYSTEMV_SHMMGR == ON
	if (size > PTRDIFF_MAX)
		size = SIZE_MAX;

	ipc_msg_t *sm_msg;
	struct shm_request *sr;
	int shm_cap;

	/* If already connected to shmmgr, skip connection */
	chcore_spin_lock(&connect_shmmgr_lock);
	if (!shmmgr_ipc_struct) {
		// printf("connect_to_shmmgr\n");
		shmmgr_ipc_struct = chcore_conn_srv(SERVER_SYSTEMV_SHMMGR);
	}
	chcore_spin_unlock(&connect_shmmgr_lock);

	/* Send IPC to shmmgr to get shm_cap */
	sm_msg =
	    ipc_create_msg(shmmgr_ipc_struct, sizeof(struct shm_request), 0);
	sr = (struct shm_request *)ipc_get_msg_data(sm_msg);
	sr->flag = flag;
	sr->key = key;
	sr->req = SHM_REQ_GET;
	sr->size = size;

	ipc_call(shmmgr_ipc_struct, sm_msg);

	shm_cap = ipc_get_msg_cap(sm_msg, 0);
	ipc_destroy_msg(sm_msg);

	if (shm_cap < 0) {
		printf("shmget fail: return %lx\n", shm_cap);
	}
	return shm_cap;
#else
	return 0;
#endif
}
