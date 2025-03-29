#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <chcore/syscall.h>
#include <chcore/thread.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <chcore/ipc.h>
#include <chcore/defs.h>
#include <chcore/memory.h>
#include <stdlib.h>
#include <stdio.h>
#include <chcore/bug.h>
#include <debug_lock.h>
#include <errno.h>
#include <assert.h>
#include <chcore-internal/lwip_defs.h>
#include <chcore/container/list.h>

#include "pthread_impl.h"
#include "fs_client_defs.h"

/*
 * **fsm_ipc_struct** is an address that points to the per-thread
 * system_ipc_fsm in the pthread_t struct.
 */
ipc_struct_t *__fsm_ipc_struct_location(void)
{
	return &__pthread_self()->system_ipc_fsm;
}

/*
 * **lwip_ipc_struct** is an address that points to the per-thread
 * system_ipc_net in the pthread_t struct.
 */
ipc_struct_t *__net_ipc_struct_location(void)
{
	return &__pthread_self()->system_ipc_net;
}

ipc_struct_t *__procmgr_ipc_struct_location(void)
{
	return &__pthread_self()->system_ipc_procmgr;
}

static int connect_system_server(ipc_struct_t *ipc_struct);

/* Interfaces for operate the ipc message (begin here) */

/*
 * ipc_msg_t is constructed on the shm pointed by
 * ipc_struct_t->shared_buf.
 * A new ips_msg will override the old one.
 */
ipc_msg_t *ipc_create_msg(ipc_struct_t *icb, u64 data_len, u64 cap_slot_number)
{
	ipc_msg_t *ipc_msg;
	u64 buf_len;

	if (unlikely(icb->conn_cap == 0)) {
		/* Create the IPC connection on demand */
		if (connect_system_server(icb) != 0)
			return NULL;
	}

	/* Grab the ipc lock before setting ipc msg */
	chcore_spin_lock(&(icb->lock));

	/* The ips_msg metadata is at the beginning of the memory */
	buf_len = icb->shared_buf_len - sizeof(ipc_msg_t);

	/*
	 * Check the total length of data and caps.
	 *
	 * The checks at client side is not for security but for preventing
	 * unintended errors made by benign clients.
	 * The server has to validate the ipc msg by itself.
	 */
	if (((data_len + sizeof(u64) * cap_slot_number) > buf_len)
	    || ((data_len + sizeof(u64) * cap_slot_number) < data_len)) {
		printf("%s failed: too long msg (the usable shm size is 0x%lx)\n",
		       __func__, buf_len);
		return NULL;
	}

	ipc_msg = (ipc_msg_t *)icb->shared_buf;
	ipc_msg->icb = icb;

	ipc_msg->data_len = data_len;
	ipc_msg->cap_slot_number = cap_slot_number;
	ipc_msg->data_offset = sizeof(*ipc_msg);
	ipc_msg->cap_slots_offset = ipc_msg->data_offset + data_len;


	/*
	 * Zeroing is not that meaningful for shared memory.
	 * If necessary, the client can explict clear the shm by itself.
	 */
	return ipc_msg;
}

char *ipc_get_msg_data(ipc_msg_t *ipc_msg)
{
	return (char *)ipc_msg + ipc_msg->data_offset;
}

int ipc_set_msg_data(ipc_msg_t *ipc_msg, void *data, u64 offset, u64 len)
{
	if ((offset + len < offset) ||
	    (offset + len > ipc_msg->data_len)) {
		printf("%s failed due to overflow.\n", __func__);
		return -1;
	}

	memcpy(ipc_get_msg_data(ipc_msg) + offset, data, len);
	return 0;
}

/* Each cap takes 8 bytes although its length is 4 bytes in fact */
static u64 *ipc_get_msg_cap_ptr(ipc_msg_t *ipc_msg, u64 cap_id)
{
	return (u64 *)((char *)ipc_msg + ipc_msg->cap_slots_offset) + cap_id;
}

u64 ipc_get_msg_cap(ipc_msg_t *ipc_msg, u64 cap_slot_index)
{
	if (cap_slot_index >= ipc_msg->cap_slot_number) {
		printf("%s failed due to overflow.\n", __func__);
		return -1;
	}
	return *ipc_get_msg_cap_ptr(ipc_msg, cap_slot_index);
}

int ipc_set_msg_cap(ipc_msg_t *ipc_msg, u64 cap_slot_index, u32 cap)
{
	if (cap_slot_index >= ipc_msg->cap_slot_number) {
		printf("%s failed due to overflow.\n", __func__);
		return -1;
	}

	*ipc_get_msg_cap_ptr(ipc_msg, cap_slot_index) = cap;
	return 0;
}

int ipc_destroy_msg(ipc_msg_t *ipc_msg)
{
	/* Release the ipc lock */
	chcore_spin_unlock(&(ipc_msg->icb->lock));

	return 0;
}


/* Interfaces for operate the ipc message (end here) */

/* Shadow thread exit routine */
int ipc_shadow_thread_exit_routine(void)
{
	pthread_detach(pthread_self());
	pthread_exit(NULL);
}

/* A register_callback thread uses this to finish a registration */
int ipc_register_cb_return(u64 server_thread_cap, u64 server_thread_exit_routine,
				   u64 server_shm_addr)
{
	return usys_ipc_register_cb_return(server_thread_cap, server_thread_exit_routine, server_shm_addr);
}

/* A register_callback thread is passive (never proactively run) */
void *register_cb(void *ipc_handler)
{
	int server_thread_cap = 0;
	u64 shm_addr;

	shm_addr = chcore_alloc_vaddr(IPC_PER_SHM_SIZE);

	// printf("[server]: A new client comes in! ipc_handler: 0x%lx\n", ipc_handler);

	/*
	 * Create a passive thread for serving IPC requests.
	 * Besides, reusing an existing thread is also supported.
	 */
	pthread_t handler_tid;
	server_thread_cap = chcore_pthread_create_shadow(
		&handler_tid, NULL, ipc_handler, (void *)NO_ARG);
	BUG_ON(server_thread_cap < 0);
	ipc_register_cb_return(server_thread_cap, (u64)ipc_shadow_thread_exit_routine, shm_addr);

	return NULL;
}

/* Register callback for single-handler-thread server */
void *register_cb_single(void *ipc_handler)
{
	static int single_handler_thread_cap = -1;
	u64 shm_addr;

	/* alloc shm_addr */
	shm_addr = chcore_alloc_vaddr(IPC_PER_SHM_SIZE);

	/* if single handler thread isn't created */
	if (single_handler_thread_cap == -1) {
		pthread_t single_handler_tid;
		single_handler_thread_cap = chcore_pthread_create_shadow(
			&single_handler_tid, NULL, ipc_handler, (void *)NO_ARG);
	}

	assert(single_handler_thread_cap > 0);
	ipc_register_cb_return(single_handler_thread_cap, (u64)NULL, shm_addr);

	return NULL;
}

/*
 * Currently, a server thread can only invoke this interface once.
 * But, a server can use another thread to register a new service.
 */
int ipc_register_server_cap(server_handler server_handler,
			    void* (*client_register_handler)(void*),
			    u32 *cap_ptr)
{
	int register_cb_thread_cap;
	int ret;

	/*
	 * Create a passive thread for handling IPC registration.
	 * - run after a client wants to register
	 * - be responsible for initializing the ipc connection
	 */
	#define ARG_SET_BY_KERNEL 0
	pthread_t handler_tid;
	register_cb_thread_cap =
		chcore_pthread_create_register_cb(&handler_tid, NULL,
					client_register_handler,
					(void *)ARG_SET_BY_KERNEL);
	BUG_ON(register_cb_thread_cap < 0);
	/*
	 * Kernel will pass server_handler as the argument for the
	 * register_cb_thread.
	 */
	ret = usys_register_server((u64)server_handler,
				    (u32)register_cb_thread_cap,
					(u64)DEFAULT_DESTRUCTOR);
	// NOTE: The cap cannot be used to connect to the server!
	if (cap_ptr)
		*cap_ptr = register_cb_thread_cap;
	if (ret != 0) {
		printf("%s failed (retval is %d)\n", __func__, ret);
	}
	return ret;
}

int ipc_register_server(server_handler server_handler,
			void* (*client_register_handler)(void*))
{
	return ipc_register_server_cap(server_handler,
				       client_register_handler,
				       NULL);
}

struct client_shm_config {
	int shm_cap;
	u64 shm_addr;
};

/*
 * A client thread can register itself for multiple times.
 *
 * The returned ipc_struct_t is from heap,
 * so the callee needs to free it.
 */
ipc_struct_t *ipc_register_client(int server_thread_cap)
{
	int conn_cap;
	ipc_struct_t *client_ipc_struct;

	struct client_shm_config shm_config;
	int shm_cap;

	/*
	 * Before registering client on the server,
	 * the client allocates the shm (and shares it with
	 * the server later).
	 *
	 * Now we used PMO_DATA instead of PMO_SHM because:
	 * - SHM (IPC_PER_SHM_SIZE) only contains one page and
	 *   PMO_DATA is thus more efficient.
	 *
	 * If the SHM becomes larger, we can use PMO_SHM instead.
	 * Both types are tested and can work well.
	 */

	shm_cap = usys_create_pmo(IPC_PER_SHM_SIZE, PMO_SHM, MALLOC_TYPE_DEFAULT);
	// shm_cap = usys_create_pmo(IPC_PER_SHM_SIZE, PMO_DATA, MALLOC_TYPE_DEFAULT);
	if (shm_cap < 0) {
		printf("usys_create_pmo ret %d\n", shm_cap);
		usys_exit(-1);
	}

	shm_config.shm_cap = shm_cap;
	shm_config.shm_addr = chcore_alloc_vaddr(IPC_PER_SHM_SIZE);

	//printf("%s: register_client with shm_addr 0x%lx\n",
	//      __func__, shm_config.shm_addr);

	while (1) {
		conn_cap = usys_register_client((u32)server_thread_cap,
						(u64)&shm_config);

		if (conn_cap == -EIPCRETRY) {
			// printf("client: Try to connect again ...\n");
			/* The server IPC may be not ready. */
			usys_yield();
		}
		else if (conn_cap < 0) {
			printf("client: %s failed (return %d), server_thread_cap is %d\n", __func__,
			       conn_cap, server_thread_cap);
			return NULL;
		}
		else {
			/* Success */
			break;
		}
	}

	client_ipc_struct = malloc(sizeof(ipc_struct_t));

	client_ipc_struct->lock = 0;
	client_ipc_struct->shared_buf = shm_config.shm_addr;
	client_ipc_struct->shared_buf_len = IPC_PER_SHM_SIZE;
	client_ipc_struct->conn_cap = conn_cap;

	return client_ipc_struct;
}

/* Client uses **ipc_call** to issue an IPC request */
s64 ipc_call(ipc_struct_t *icb, ipc_msg_t *ipc_msg)
{
	s64 ret;

	if (unlikely(icb->conn_cap == 0)) {
		/* Create the IPC connection on demand */
		if ((ret = connect_system_server(icb)) != 0)
			return ret;
	}

	do {
		ret = usys_ipc_call(icb->conn_cap, (u64)ipc_msg,
			    ipc_msg->cap_slot_number);
	} while (ret == -EIPCRETRY);

	return ret;
}

/* Server uses **ipc_return** to finish an IPC request */
void ipc_return(ipc_msg_t *ipc_msg, long ret)
{
	ipc_msg->cap_slot_number = 0;
	usys_ipc_return((u64)ret, 0);
}

/*
 * IPC return and copy back capabilities.
 * XXX: Use different ipc return interface because cap_slot_number
 * is valid only when we have cap to return. So we need to reset it to
 * 0 in ipc_return which has no cap to return.
 */
void ipc_return_with_cap(ipc_msg_t *ipc_msg, long ret)
{
	usys_ipc_return((u64)ret, ipc_msg->cap_slot_number);
}

int simple_ipc_forward(ipc_struct_t *ipc_struct, void *data, int len)
{
	ipc_msg_t *ipc_msg;
	int ret;

	ipc_msg = ipc_create_msg(ipc_struct, len, 0);
	ipc_set_msg_data(ipc_msg, data, 0, len);
	ret = ipc_call(ipc_struct, ipc_msg);
	ipc_destroy_msg(ipc_msg);

	return ret;
}

static void ipc_struct_copy(ipc_struct_t *dst, ipc_struct_t *src)
{
	dst->conn_cap = src->conn_cap;
	dst->shared_buf = src->shared_buf;
	dst->shared_buf_len = src->shared_buf_len;
	dst->lock = src->lock;
}

extern ipc_struct_t *procmgr_ipc_struct;
extern int fs_server_cap;
extern int lwip_server_cap;
extern int procmgr_server_cap;
// extern struct list_head fs_ipc_pool;

int reconnect_to_system_servers(u64 new_fs_cap, u64 new_lwip_cap, u64 new_procmgr_cap)
{
	procmgr_server_cap = new_procmgr_cap;
	procmgr_ipc_struct->conn_cap = 0;
	procmgr_ipc_struct->server_id = PROC_MANAGER;

	lwip_server_cap = new_lwip_cap;
	lwip_ipc_struct->conn_cap = 0;
	lwip_ipc_struct->server_id = NET_MANAGER;
	
	fsm_server_cap = new_fs_cap;
	fsm_ipc_struct->conn_cap = 0;
	fsm_ipc_struct->server_id = FS_MANAGER;

#if 0
	init_list_head(&fs_ipc_pool);

	struct fs_ipc_pool_node *iter;
	for_each_in_list(iter, struct fs_ipc_pool_node, node, &fs_ipc_pool) {
		// printf("old fs cap = %lx,old conn cap = %lx, serverid = %d\n",
		// iter->fs_cap,
		// iter->_fs_ipc_struct->conn_cap,
		// iter->_fs_ipc_struct->server_id);

		iter->mount_id = -1;
		iter->_fs_ipc_struct->conn_cap = 0;
	}

#endif	// printf("reconnect cap, ps=%lx,ls=%lx,fs=%lx\n",new_procmgr_cap,new_lwip_cap,new_fs_cap);
}

static int connect_system_server(ipc_struct_t *ipc_struct)
{
	ipc_struct_t *tmp;

	switch (ipc_struct->server_id) {
	case FS_MANAGER: {
		tmp = ipc_register_client(fsm_server_cap);
		if (tmp == NULL) {
			printf("%s: failed to connect FS\n", __func__);
			return -1;
		}
		break;
	}
	case NET_MANAGER: {
		tmp = ipc_register_client(lwip_server_cap);
		if (tmp == NULL) {
			printf("%s: failed to connect NET\n", __func__);
			return -1;
		}
		break;
	}
	case PROC_MANAGER: {
		tmp = ipc_register_client(procmgr_server_cap);
		if (tmp == NULL) {
			printf("%s: failed to connect PROCMGR\n", __func__);
			return -1;
		}
		break;
	}
	default:
		printf("%s: unsupported system server id %d\n",
		       __func__, ipc_struct->server_id);
		return -1;
	}

	/* Copy the newly allocated ipc_struct to the per_thread ipc_struct */
	ipc_struct_copy(ipc_struct, tmp);
	free(tmp);

	return 0;
}
