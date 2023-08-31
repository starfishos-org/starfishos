#pragma once

#include <object/thread.h>

enum config_type {
	IPC_SERVER_HANDLER = 1,
	IPC_SERVER_REGISTER_CB,
	IPC_SERVER
};

struct ipc_config {
	u64 config_type;
};

/*
 * An ipc_server_handler_thread has such config which stores its information.
 * Note that the ipc_server_handler_thread is used for handling IPC requests.
 */
struct ipc_server_handler_config {
	u64 config_type;
	/* Avoid invoking the same handler_thread concurrently */
	struct lock ipc_lock;

	/* PC */
	u64 ipc_routine_entry;
	/* SP */
	u64 ipc_routine_stack;
	/* Entry point of shadow thread exit routine */
	u64 ipc_exit_routine_entry;

	/*
	 * Record which connection uses this handler thread now.
	 * Multiple connection can use the same handler_thread.
	 */
	struct ipc_connection *active_conn;
};

/*
 * An ipc_server_register_cb_thread has such config which stores its information.
 * This thread is used for handling IPC registration.
 */
struct ipc_server_register_cb_config {
	u64 config_type;
	struct lock register_lock;
	/* PC */
	u64 register_cb_entry;
	/* SP */
	u64 register_cb_stack;

	/* The caps for the connection currently building */
	int conn_cap_in_client;
	/* Not used now (can be exposed to server in future) */
	int conn_cap_in_server;
	int shm_cap_in_server;
};

/*
 * An ipc_server_thread which invokes "reigster_server" has such config.
 * This thread, which declares an IPC service in the server process,
 * will be exposed to clients. Then, clients invokes "register_client"
 * with such ipc_server_thread.
 */
struct ipc_server_config {
	u64 config_type;
	/* Callback_thread for handling client registration */
	struct thread *register_cb_thread;

	/* Record the argument from the server thread */
	u64 declared_ipc_routine_entry;
};

/*
 * Each connection owns one shm for exchanging data between client and server.
 * Client process registers one PMO_SHM and copies the shm_cap to the server.
 * But, client and server can map the PMO_SHM at different addresses.
 */
struct shm_for_ipc_connection {
	/*
	 * The starting address of the shm in the client process's vmspace.
	 * uaddr: user-level virtual address.
	 */
	u64 client_shm_uaddr;

	/* The starting address of the shm in the server process's vmspace. */
	u64 server_shm_uaddr;
	u64 shm_size;

	/* For resource recycle */
	int shm_cap_in_client;
	int shm_cap_in_server;
};

struct ipc_connection {
	/*
	 * current client who uses this connection.
	 * Note that all threads in the client process can use this connection.
	 */
	struct thread *current_client_thread;

	/*
	 * server_handler_thread is always fixed after establishing the
	 * connection.
	 * i.e., ipc_server_handler_thread
	 */
	struct thread *server_handler_thread;

	/*
	 * Identification of the client (cap_group).
	 * This badge is always fixed with the ipc_connection and
	 * will be transferred to the server during each IPC.
	 * Thus, the server can identify different client processes.
	 *
	 * NOTE: an connection cannot be shared between multiple clients
	 * TODO: disable cap_copy on TYPE_CONNECTION.
	 */
	u64 client_badge;

	/* XXX: for temporary use of return cap from server to client */
	struct ipc_msg *user_ipc_msg;

	struct shm_for_ipc_connection shm;

	/* For resource recycle */
	struct lock ownership;
#ifdef CKPT_CONNECTION_LAZY_COPY
	struct lock copylock;
#endif

	int conn_cap_in_client;
	int conn_cap_in_server;
	int is_valid;
};

#define INVALID (0)
#define VALID (1)

/*
 * TODO: use uapi for shared data structure declaration
 * between kernel and user space.
 */
struct ipc_msg {
	u64 data_len;
	u64 cap_slot_number;
	u64 data_offset;
	u64 cap_slots_offset;
};


/* IPC related system calls */
u64 sys_register_server(u64 ipc_rountine, u64 register_cb_cap);
u32 sys_register_client(u32 server_cap, u64 vm_config_ptr);
void sys_ipc_register_cb_return(u64, u64, u64);

u64 sys_ipc_call(u32 conn_cap, struct ipc_msg *ipc_msg, u64 cap_num);
void sys_ipc_return(u64 ret, u64 cap_num);
u64 sys_ipc_send_cap(u32 conn_cap, u32 send_cap);
