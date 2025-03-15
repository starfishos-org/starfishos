#pragma once

#include <chcore/type.h>
#include <chcore/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts from 1 because the uninitialized value is 0 */
enum system_server_identifier {
	FS_MANAGER = 1,
	NET_MANAGER,
	PROC_MANAGER,
};

/*
 * ipc_struct is created in **ipc_register_client** and
 * thus only used at client side.
 */
typedef struct ipc_struct {
	/* Connection_cap: used to call a server */
	u64 conn_cap;
	/* Shared memory: used to create ipc_msg (client -> server) */
	u64 shared_buf;
	u64 shared_buf_len;

	/* A spin lock: used to coordinate the access to shared memory */
	volatile int lock;
	enum system_server_identifier server_id;
} ipc_struct_t;

extern int fsm_server_cap;
extern int lwip_server_cap;
extern int procmgr_server_cap;
extern int init_process_in_lwip;
extern int init_lwip_lock;

/* ipc_struct for invoking system servers.
 * fsm_ipc_struct and lwip_ipc_struct are two addresses.
 * They can be used like **const** pointers.
 *
 * If a system server is related to the scalability (multi-threads) of applications,
 * we should use the following way to make the connection with it as per-thread.
 *
 * For other system servers (e.g., process manager), it is OK to let multiple threads
 * share a same connection.
 */
ipc_struct_t *__fsm_ipc_struct_location();
ipc_struct_t *__net_ipc_struct_location();
ipc_struct_t *__procmgr_ipc_struct_location();
#define fsm_ipc_struct   (__fsm_ipc_struct_location())
#define lwip_ipc_struct (__net_ipc_struct_location())
#define procmgr_ipc_struct (__procmgr_ipc_struct_location())

/* ipc_msg is located at ipc_struct->shared_buf. */
typedef struct ipc_msg {
	u64 data_len;
	/*
	 * cap_slot_number represents the number of caps of the ipc_msg.
	 * This is useful for both sending IPC and returning from IPC.
	 * When calling ipc_return, cap_slot_number will be set 0 automatically,
	 * indicating that no cap will be sent.
	 * If you want to send caps when returning from IPC,
	 * use ipc_return_with_cap.
	 */
	u64 cap_slot_number;
	u64 data_offset;
	u64 cap_slots_offset;

	/* icb: ipc control block (not needed by the kernel) */
	ipc_struct_t *icb;
} ipc_msg_t;

#define IPC_SHM_AVAILABLE (IPC_PER_SHM_SIZE - sizeof(ipc_msg_t))

/*
 * server_handler is an IPC routine (can have two arguments):
 * first is ipc_msg and second is client_badge.
 */
typedef void (*server_handler)();

/* Registeration interfaces */
ipc_struct_t *ipc_register_client(int server_thread_cap);
ipc_struct_t *ipc_register_fs_client(int target_machine_id);

void *register_cb(void *ipc_handler);
void *register_cb_single(void *ipc_handler);
#define DEFAULT_CLIENT_REGISTER_HANDLER register_cb
int ipc_register_server(server_handler server_handler,
			void *(*client_register_handler)(void *));
int ipc_register_server_cap(server_handler server_handler,
			    void *(*client_register_handler)(void *),
			    u32 *cap_ptr);

/* IPC message operating interfaces */
ipc_msg_t *ipc_create_msg(ipc_struct_t *icb, u64 data_len, u64 cap_slot_number);
char *ipc_get_msg_data(ipc_msg_t *ipc_msg);
u64 ipc_get_msg_cap(ipc_msg_t *ipc_msg, u64 cap_id);
int ipc_set_msg_data(ipc_msg_t *ipc_msg, void *data, u64 offset, u64 len);
int ipc_set_msg_cap(ipc_msg_t *ipc_msg, u64 cap_slot_index, u32 cap);
int ipc_destroy_msg(ipc_msg_t *ipc_msg);

/* IPC issue/finish interfaces */
s64 ipc_call(ipc_struct_t *icb, ipc_msg_t *ipc_msg);
void ipc_return(ipc_msg_t *ipc_msg, long ret);
void ipc_return_with_cap(ipc_msg_t *ipc_msg, long ret);

int simple_ipc_forward(ipc_struct_t *ipc_struct, void *data, int len);

/*
 * Magic number for coordination between client and server:
 * the client should wait until the server has reigsterd the service.
 */
#define NONE_INFO ((void *)(-1UL))

#ifdef __cplusplus
}
#endif
