#include <chcore/ipc.h>
#include <chcore/pmu.h>
#include <stdio.h>
#include <pthread.h>
#include <chcore/syscall.h>
#include <errno.h>
#include <chcore/string.h>
#include <time.h>

#include <lwip/debug.h>
#include <lwip/init.h>

#include <lwip/mem.h>
#include <lwip/memp.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#include <lwip/ip.h>
#include <lwip/ip4_frag.h>
#include <lwip/sockets.h>
#include <lwip/stats.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <netif/etharp.h>

#include <lwip/apps/snmp.h>
#include <lwip/apps/snmp_mib2.h>

#define LWIP_SRC	/* needed by lwip_def */
#include <chcore-internal/lwip_defs.h>

// #define DEBUG

#define PREFIX "[lwip]"

#define info(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#define error(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define debug(fmt, ...) printf(PREFIX " " fmt, ##__VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

// Information for local socket forwarding
// Diagram:
// Normal connection:
//   TCPServer <-> IPServer <-> TCPClient
// Socket forwarding:
//   TCPServer <-> IPServer <-> TCPClient
//      ^  ^                     | ^
//      |  |                     | |
//      |  +-IPC to setup ringbuf+ |
//      |                          |
//      +----Two shared ringbufs---+
//  Notes:
//   1. TCPServer needs to setup as a IPC server;
//   2. TCPServer registers its information (IPC server cap) when it binds to IPServer;
//   3. IPServer records the TCPServer's info. (IP, port, and cap) in `bind_info_list`;
//   3. When a TCPClient wants to connect, the IPServer looks up in `bind_info_list` to
//        check whether the TCPClient wants to connect a local TCPServer;
//        If it's local, the TCPClient's info. (IP and port) is published in
//        `local_conn_info_list`. Then IPServer will wait here until the connection is;
//        accepted by the server part of IPServer.
//   4. When the IPServer accepts a connection, it looks up in `local_conn_info_list`
//        to know the other end of the connection is a local client by comparing IP and port;
//        If it is, the IPServer will set the server's fd's `conn_id` in the clinet's
//        `local_conn_info`.
//        Note that the server's fd's `conn_id` is important.
//   5. After getting the server's fd's `conn_id`, IPServer at the TCPClient side responds
//        the TCPClient with the server's IPC server cap and the server's fd's `conn_id`;
//   6. When TCPClient gets the server's IPC server cap and the server's fd's `conn_id`,
//        it setups two PMOs ringbuffers, one is `wr_ringbuf` for send data and the other
//        is `rd_ringbuf` to receive data. The two ringbufs are recorded in the fd structure
//        of the fd used to connect to TCPServer. TCPClient then connects as an IPC client
//        to the server's server, sends an IPC of type `LWIP_LOCAL_SOCKET_FORWARD_CONNECT`
//        with the two PMOs and the `conn_id`.
//   7. TCPServer's IPC server gets the two PMOs and the `conn_id`. By finding the `conn_id`
//        in the fd table, TCPServer can know which fd is connected with this TCPClient.
//        TCPServer then associate the two PMO ringbuffers in the fd's structure.
//        Note that the TCPClient's `wr_ringbuf` is set as the TCPServer's `rd_ringbuf`.
//        The same applies for `rd_ringbuf`.
//   8. Now all the setup is done. The TCPServer and the TCPClient can send/receive data
//        with the two PMO ringbuffers.
//      There is a header in each ringbuffer to control the use of ringbuf, please refer
//        to the socket.c file.

// Records of binding informations
struct bind_info {
	char ip[16];
	int port;
	int server_cap;  // TCP Server's IPC server cap
	struct bind_info *next;
};
struct bind_info *bind_info_list = NULL;

// Records of local connect request information
// The client needs to get a token (the server's accepted fd's conn_id) so that
//   the server can recognize the client's connection in IPC requests.
enum local_conn_state {
	LOCAL_CONN_UNKNOWN = 0,
	LOCAL_CONN_SENDING = 1,
	LOCAL_CONN_SENT = 2,
	LOCAL_CONN_ACCEPTED = 3,
	LOCAL_CONN_PAIRED = 4, // for lazy reclaimation
};
struct local_conn_info {
	// the client should set ip and port
	char ip[16];
	int port;
	// the server should set conn_id in response
	int conn_id;  // the token, it is the server's accepted fd's conn_id
	// the client is responsible for allocating the info and the server for freeing
	enum local_conn_state state;
	struct local_conn_info *next;
};
struct local_conn_info *local_conn_info_list = NULL;
// I intended to use a spinlock here, but we need to call free while holding the list...
// Of course this can be optimized, but I am lazy...
pthread_mutex_t local_conn_info_list_lock = PTHREAD_MUTEX_INITIALIZER;




/* (manual) host IP configuration */
static ip4_addr_t ipaddr, netmask, gw;
/* nonstatic debug cmd option, exported in lwipopts.h */
unsigned char debug_flags;

#define MAX_SOCK_PER_CLIENT	4096
struct sockinfo_node {
	u64 badge;
	int socket_table[MAX_SOCK_PER_CLIENT];
	struct sockinfo_node *next;
};

/* Node list header */
struct sockinfo_node head = {
	.badge = -1,
	.socket_table = { -1 },
	.next = NULL
};

static pthread_mutex_t sockinfo_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sockinfo_node *head_ptr = &head;

/* XXX: free the sockinfo node before process exit */
/* This function should be in the critical section */
int *register_sockinfo_node(u64 badge)
{
	struct sockinfo_node *cur = head_ptr;
	struct sockinfo_node *tail = NULL;
	int *table;

	/* check if cap exist (cap can be reused) */
	while (cur != NULL) {
		if (badge == cur->badge) {
			/* already has such node */
			table = cur->socket_table;
			goto out;
		}
		tail = cur;
		cur = cur->next;
	}

	/*
	 * This operation should be done when register connection, thus no other thread
	 * which holds the same cap will register sockinfo_node concurrently.
	 */
	cur = (struct sockinfo_node *)malloc(sizeof(struct sockinfo_node));
	memset(cur->socket_table, -1, sizeof(cur->socket_table));
	cur->next = NULL;
	cur->badge = badge;
	tail->next = cur;
	table = cur->socket_table;
out:
	return table;
}

/*
 * Find the socket fd through client badge and the conn_id
 */
int get_socket(u64 badge, int conn_id)
{
	struct sockinfo_node *cur = head_ptr;
	int ret = 0;

	if (conn_id < 0 || conn_id > MAX_SOCK_PER_CLIENT)
		return -EINVAL;

	pthread_mutex_lock(&sockinfo_lock);
	while (cur != NULL) {
		if (badge == cur->badge) {
			/* find socket_fd in socket_table */
			ret = cur->socket_table[conn_id];
			goto out;
		}
		cur = cur->next;
	}
	ret = -EINVAL;
out:
	pthread_mutex_unlock(&sockinfo_lock);
	return ret;
}

/*
 * Set socket fd and allocate a new conn_id
 * Return the new conn_id
 */
int set_socket(u64 badge, int socket_fd)
{
	struct sockinfo_node *cur = head_ptr;
	int *table = NULL;
	int i = 0, ret = 0;

	pthread_mutex_lock(&sockinfo_lock);
	while (cur != NULL) {
		if (badge == cur->badge) {
			/* already has such node */
			table = cur->socket_table;
			break;
		}
		cur = cur->next;
	}

	if (!table) {
		/* create badge -> socket table
		 * register_sockinfo_node should be in critical section */
		table = register_sockinfo_node(badge);
	}

	/* Find a suitable conn_id */
	for (i = 0; i < MAX_SOCK_PER_CLIENT; i++) {
		if (table[i] == -1) {
			table[i] = socket_fd;
			ret = i;
			goto out;
		}
	}
	ret = -ENOSPC;
out:
	pthread_mutex_unlock(&sockinfo_lock);
	return ret;
}

/*
 * Free the conn_id
 */
int free_socket(u64 badge, int conn_id)
{
	struct sockinfo_node *cur = head_ptr;
	int *table = NULL;
	int ret = 0;

	pthread_mutex_lock(&sockinfo_lock);
	while (cur != NULL) {
		if (badge == cur->badge) {
			/* already has such node */
			table = cur->socket_table;
			break;
		}
		cur = cur->next;
	}

	if (table) {
		table[conn_id] = -1;
		ret = 0;
	} else {
		/* cap - socket node should be created when register lwip server */
		printf("FATAL BUG: No badge - socket node found!\n");
		ret = -EINVAL;
	}
	pthread_mutex_unlock(&sockinfo_lock);
	return ret;
}

void lwip_dispatch(ipc_msg_t * ipc_msg, u64 client_badge)
{
	int ret = 0, i = 0;
	int socket = 0, accpet_fd = 0, /*port = 0,*/ flags = 0, backlog = 0;
	int cmd = 0, val = 0;
	int domain = 0, protocol = 0, type = 0, level = 0, optname = 0;
	socklen_t len = 0, alen = 0, optlen = 0, namelen = 0;
	struct msghdr *msg;
	int shared_pmo = 0;
	struct sockaddr target;
	bool has_cap = false;

	/* lr->req takes 4 bytes. */
	if (ipc_msg->data_len >= 4) {
		struct lwip_request *lr =
		    (struct lwip_request *)ipc_get_msg_data(ipc_msg);
		struct sockaddr_in *sockaddr = NULL;
		struct bind_info *bi_node = NULL;

		switch (lr->req) {
		case LWIP_CREATE_SOCKET:
			debug("LWIP_CREATE_SOCKET\n");
			domain = lr->args[0];
			type = lr->args[1];
			protocol = lr->args[2];
			socket = lwip_socket(domain, type, protocol);
			/* only set socket when succ */
			ret = (socket >= 0)? set_socket(client_badge, socket):socket;
			debug("LWIP_CREATE_SOCKET return %d\n", ret);
			break;
		case LWIP_SOCKET_BIND:
			debug("LWIP_SOCKET_BIND\n");
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];
			sockaddr = (struct sockaddr_in *)lr->data;
			// Local Socket Forwarding:
			// record the bind info
			bi_node = malloc(sizeof(struct bind_info));
			lwip_inet_ntop(AF_INET, (char *)&sockaddr->sin_addr,
				       bi_node->ip, sizeof(bi_node->ip));
			bi_node->port = ntohs(sockaddr->sin_port);
			// bind request should transfer the TCPServer's cap to me
			bi_node->server_cap = ipc_get_msg_cap(ipc_msg, 0);
			bi_node->next = bind_info_list;
			bind_info_list = bi_node;

			debug("BIND to %16s:%d with tcpservercap=%d\n",
			       bi_node->ip, bi_node->port, bi_node->server_cap);
			ret =
			    lwip_bind(socket, (struct sockaddr *)lr->data, len);
			break;
		case LWIP_SOCKET_RECV:
			/* here we will call recvfrom rather than recv */
			debug("LWIP_SOCKET_RECV\n");
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];
			flags = lr->args[2];
			alen = lr->args[3];

			ret = lwip_recvfrom(socket, lr->data, len, flags, &target, &alen);
			if (alen > 0)
				memcpy(((char *)lr->data + len), &target, alen);
			debug("LWIP_SOCKET_RECV return %d\n", ret);
			lr->args[3] = alen;

			break;
		case LWIP_SOCKET_READ:
			debug("LWIP_SOCKET_READ\n");
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];
			ret = lwip_read(socket, lr->data, len);
			break;
		case LWIP_SOCKET_RMSG:
			debug("LWIP_SOCKET_RMSG\n");
			struct sockaddr target;
			socket = get_socket(client_badge, lr->args[0]);
			flags = lr->args[1];
			len = lr->args[2];
			if (len > LWIP_DATA_LEN) {
				if (ipc_msg->cap_slot_number != 1) {
					ret = -EINVAL;
					goto out;
				}
				shared_pmo = ipc_get_msg_cap(ipc_msg, 0);
				msg = malloc(len);
				/* XXX: can be optimized */
				usys_read_pmo(shared_pmo, 0, msg, len);
				update_msg_ptr(msg);
				ret = lwip_recvmsg(socket, msg, flags);
				usys_write_pmo(shared_pmo, 0, msg, len);
				free(msg);
			} else {
				msg = (struct msghdr *)lr->data;
				update_msg_ptr(msg);
				ret = lwip_recvmsg(socket, msg, flags);
			}

			break;
		case LWIP_SOCKET_SEND:
			/* here we will call sendto rather than send */
			debug("LWIP_SOCKET_SEND\n");
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];
			flags = lr->args[2];
			alen = (socklen_t) lr->args[3];
			if (alen > 0)
				memcpy(&target, ((char *)lr->data + len), alen);
			ret =
			    lwip_sendto(socket, lr->data, len, flags, &target, alen);
			debug("LWIP_SOCKET_SEND return %d\n", ret);
			break;
		case LWIP_SOCKET_WRITE:
			socket = get_socket(client_badge, lr->args[0]);
			debug("LWIP_SOCKET_WRITE socket %d badge %ld args %ld\n", socket, client_badge, lr->args[0]);
			len = lr->args[1];
			ret = lwip_write(socket, lr->data, len);
			debug("LWIP_SOCKET_WRITE return %d %d\n", ret, errno);
			break;
		case LWIP_SOCKET_SMSG:
			debug("LWIP_SOCKET_SMSG\n");
			socket = get_socket(client_badge, lr->args[0]);
			flags = lr->args[1];
			len = lr->args[2];
			if (len > LWIP_DATA_LEN) {
				if (ipc_msg->cap_slot_number != 1) {
					ret = -EINVAL;
					goto out;
				}
				shared_pmo = ipc_get_msg_cap(ipc_msg, 0);
				msg = malloc(len);
				usys_read_pmo(shared_pmo, 0, msg, len);
				update_msg_ptr(msg);
				ret = lwip_sendmsg(socket, msg, flags);
				free(msg);
			} else {
				msg = (struct msghdr *)lr->data;
				update_msg_ptr(msg);
				ret = lwip_sendmsg(socket, msg, flags);
			}
			break;
		case LWIP_SOCKET_LIST:
			debug("LWIP_SOCKET_LIST\n");
			socket = get_socket(client_badge, lr->args[0]);
			backlog = lr->args[1];
			ret = lwip_listen(socket, backlog);
			break;
		case LWIP_SOCKET_CONN: {
			char ip[16];
			int port;
			debug("LWIP_SOCKET_CONN\n");
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];

			sockaddr = (struct sockaddr_in *)lr->data;
			// Local Socket Forwarding:
			// extract the info. to find a match
			lwip_inet_ntop(AF_INET, (char *)&sockaddr->sin_addr,
				       ip, sizeof(ip));
			port = ntohs(sockaddr->sin_port);
			// printf("connecting to %s:%d\n", ip, port);
			// send the TCPServer's cap to client
			for (bi_node = bind_info_list;
			     bi_node;
			     bi_node = bi_node->next) {
				debug("server listing in CONN:"
				       " to %16s:%d with tcpservercap=%d\n",
				       bi_node->ip, bi_node->port,
				       bi_node->server_cap);

				/* "0.0.0.0" for all local connections */
				if ((strncmp(bi_node->ip, "0.0.0.0", 7) == 0) &&
						port == bi_node->port) {
					debug("server match found in CONN:"
				       " to %16s:%d with tcpservercap=%d\n",
				       bi_node->ip, bi_node->port,
				       bi_node->server_cap);
					ipc_msg->cap_slot_number = 1;
					ipc_set_msg_cap(ipc_msg, 0, bi_node->server_cap);
					has_cap = true;
					break;
				}

				if (strncmp(ip, bi_node->ip, 16) != 0 ||
				    port != bi_node->port)
					continue;
				// match!
				debug("server match found in CONN:"
				       " to %16s:%d with tcpservercap=%d\n",
				       bi_node->ip, bi_node->port,
				       bi_node->server_cap);
				ipc_msg->cap_slot_number = 1;
				ipc_set_msg_cap(ipc_msg, 0, bi_node->server_cap);
				has_cap = true;
				break;
			}
			if (has_cap) {
				// We need to get the relation between each connect-accept pair.
				// To do so, we need to publish myself to a global pool before the
				//   lwip_connect so that server can find me when it accepts my
				//   connection.
				// As there could be multiple (local or remote) clients connect to
				//   the server, I need to set my address in the global pool, via
				//   which the server can know it's me.
				// But I get my address (and port) after lwip_connect, I will first
				//   publish myself in the pool and set my address and port afterwards.
				// When the server finds me without the address and port set, it should
				//   wait for me.
				struct sockaddr_storage myaddr;
				socklen_t myaddr_len = sizeof(myaddr) ;
				// Publsh myself to the global list so that the server knows to wait for me
				struct local_conn_info *info = malloc(sizeof(*info));
				info->state = LOCAL_CONN_SENDING;
				pthread_mutex_lock(&local_conn_info_list_lock);
				info->next = local_conn_info_list;
				local_conn_info_list = info;
				pthread_mutex_unlock(&local_conn_info_list_lock);

				lwip_connect(socket, (struct sockaddr *)lr->data, len);
				/* ret=-1 here, but we later loop waiting for accpect, 
				 * so ignore it */

				// Set my address/port, it seems that we don't need acquire the lock.
				int ret2 = lwip_getsockname(socket,
							    (struct sockaddr *)&myaddr,
							    &myaddr_len);
				if (ret2 != 0) {
					printf("getsocketname error!\n");
					usys_exit(-1);
				}
				if (myaddr.ss_family != AF_INET) {
					printf("family error %d!\n", myaddr.ss_family);
					usys_exit(-1);
				}
				lwip_inet_ntop(AF_INET,
					       (char *)&((struct sockaddr_in *)&myaddr)->sin_addr,
					       info->ip,
					       sizeof(info->ip));
				info->port = ntohs(((struct sockaddr_in *)&myaddr)->sin_port);
				debug("my accepted fd's ip=%s port=%d\n", info->ip, info->port);
				__atomic_thread_fence(__ATOMIC_SEQ_CST);
				info->state = LOCAL_CONN_SENT;
				int server_conn_id = -1;
				while (server_conn_id == -1) {
					pthread_mutex_lock(&local_conn_info_list_lock);
					if (*(volatile int *)(&info->state) == LOCAL_CONN_ACCEPTED) {
						server_conn_id = info->conn_id;
						info->state = LOCAL_CONN_PAIRED;
					}
					pthread_mutex_unlock(&local_conn_info_list_lock);
				}
				lr->args[2] = server_conn_id; // Return the token (conn_id) to the client
				debug("|| server_conn_id = %d\n", server_conn_id);

				/* Set connection OK and Return */
				ret = 0;
			} else {
				ret = lwip_connect(socket, (struct sockaddr *)lr->data, len);
			}
			break;
		}
		case LWIP_SOCKET_ACPT: {
			debug("LWIP_SOCKET_ACPT\n");
			char ip[16];
			int port;
			socket = get_socket(client_badge, lr->args[0]);
			len = lr->args[1];
			accpet_fd = lwip_accept(socket,
					(struct sockaddr *)lr->data, &len);
			/* only set socket when succ */
			ret = (accpet_fd >= 0)? set_socket(client_badge, accpet_fd):accpet_fd;
			/* set len to pass it back */
			lr->args[1] = len;

			lwip_inet_ntop(AF_INET,
				       (char *)&((struct sockaddr_in *)lr->data)->sin_addr,
				       ip,
				       sizeof(ip));
			port = ntohs(((struct sockaddr_in *)lr->data)->sin_port);
			pthread_mutex_lock(&local_conn_info_list_lock);
			{ 				struct local_conn_info *info;
				// Reclaim
				while (local_conn_info_list &&
				       local_conn_info_list->state == LOCAL_CONN_PAIRED) {
					void *a = (void *)local_conn_info_list;
					local_conn_info_list = local_conn_info_list->next;
					free(a);
				}
				// It's okay to use loop in the mutex lock since the connection
				//   part won't use lock to set the address and port.
				for (info = local_conn_info_list;
				     info;
				     info = info->next) {
					// Reclaim
					while (info->next && info->next->state == LOCAL_CONN_PAIRED) {
						void *a = (void *)info->next;
						info->next = info->next->next;
						free(a);
					}
					// Do the work
					if (info->state == LOCAL_CONN_ACCEPTED)
						continue;
					while (info->state == LOCAL_CONN_SENDING) {
						usys_yield();
					}
					if (info->state == LOCAL_CONN_SENT) {
						if (strncmp(ip, info->ip, 16) != 0 ||
						    port != info->port)
							continue;
						// Match!
						// printf("|| matched, giving info->conn_id = %d\n", ret);
						info->conn_id = ret;  // ret is the accepted fd's conn_id
						info->state = LOCAL_CONN_ACCEPTED;
					} else {
						printf("Reach unreachable here!\n");
						usys_exit(-1);
					}
				}
			}
			pthread_mutex_unlock(&local_conn_info_list_lock);
			break;
		}
		case LWIP_SOCKET_CLSE:
			debug("LWIP_SOCKET_CLSE\n");
			socket = get_socket(client_badge, lr->args[0]);
			ret = lwip_close(socket);
			if (ret == 0)
				free_socket(client_badge, lr->args[0]);
			break;
		case LWIP_SOCKET_SOPT:
			debug("LWIP_SOCKET_SOPT\n");
			socket = get_socket(client_badge, lr->args[0]);
			level = lr->args[1];
			optname = lr->args[2];
			optlen = lr->args[3];
			ret = lwip_setsockopt(socket, level, optname,
						    lr->data, optlen);
			break;
		case LWIP_REQ_FCNTL:
			debug("LWIP_REQ_FCNTL\n");
			socket = get_socket(client_badge, lr->args[0]);
			cmd = lr->args[1];
			val = lr->args[2];
			ret = lwip_fcntl(socket, cmd, val);
			break;
		case LWIP_SOCKET_GOPT:
			debug("LWIP_SOCKET_GOPT\n");
			socket = get_socket(client_badge, lr->args[0]);
			level = lr->args[1];
			optname = lr->args[2];
			optlen = lr->args[3];
			ret = lwip_getsockopt(socket, level, optname,
						    lr->data, &optlen);
			lr->args[3] = optlen;
			break;
		case LWIP_SOCKET_NAME:
			debug("LWIP_SOCKET_NAME\n");
			socket = get_socket(client_badge, lr->args[0]);
			namelen = lr->args[1];
			if (namelen == -1)
				ret = -EINVAL;
			else
				ret =
				    lwip_getsockname(socket,
						     (struct sockaddr *)lr->
						     data, &namelen);
			lr->args[1] = namelen;
			break;
		case LWIP_SOCKET_PEER:
			debug("LWIP_SOCKET_PEER\n");
			socket = get_socket(client_badge, lr->args[0]);
			namelen = lr->args[1];
			if (namelen == -1)
				ret = -EINVAL;
			else
				ret =
				    lwip_getpeername(socket,
						     (struct sockaddr *)lr->
						     data, &namelen);
			lr->args[1] = namelen;
			break;
		case LWIP_SOCKET_STDW:
			debug("LWIP_SOCKET_STDW\n");
			socket = get_socket(client_badge, lr->args[0]);
			ret = lwip_shutdown(socket, lr->args[1]);
			break;
		case LWIP_REQ_SOCKET_POLL: {
			debug("LWIP_SOCKET_POLL\n");
			unsigned long nfds = lr->args[1];
			unsigned long timeout = lr->args[2];
			struct pollfd *fd_iter;

			for (i = 0; i < nfds; i++) {
				fd_iter = ((struct pollfd *)lr->data) + i;
				fd_iter->fd = get_socket(client_badge, fd_iter->fd);
			}
			ret = lwip_poll((struct pollfd *)lr->data, nfds, timeout);
			break;
		}
		case LWIP_SOCKET_IOCTL:
			debug("LWIP_SOCKET_IOCTL\n");
			socket = get_socket(client_badge, lr->args[0]);
			long cmd = lr->args[1];
			ret = lwip_ioctl(socket, cmd, lr->data);
			break;
		case LWIP_INTERFACE_ADD: {
			debug("LWIP_INTERFACE_ADD\n");
			extern int add_interface(ipc_msg_t *ipc_msg, struct lwip_request *lr);
			ret = add_interface(ipc_msg, lr);
			break;
		}
		default:
			printf("lwip not impl req:%d\n", lr->req);
			ret = -EINVAL;
		}

		/* LWIP should return errno to the caller. */
		if (ret < 0) ret = -errno;

	} else {
		/* No OP NUMBER */
		ret = -EINVAL;
	}
out:
	if (has_cap)
		ipc_return_with_cap(ipc_msg, ret);
	else
		ipc_return(ipc_msg, ret);
	return;
}

err_t netif_loopif_init(struct netif *netif);

int main(int argc, char *argv[], char *envp[])
{
	struct netif netif;
	char ip_str[16] = { 0 }, nm_str[16] = { 0 }, gw_str[16] = { 0 };

	/* startup defaults (may be overridden by one or more opts) */
	IP4_ADDR(&gw, 192, 168, 0, 1);
	IP4_ADDR(&ipaddr, 192, 168, 0, 3);
	IP4_ADDR(&netmask, 255, 255, 255, 0);

	/* use debug flags defined by debug.h */
	debug_flags = LWIP_DBG_OFF;

	debug_flags |= (LWIP_DBG_ON | LWIP_DBG_TRACE | LWIP_DBG_STATE |
			LWIP_DBG_FRESH | LWIP_DBG_HALT);

	strlcpy(ip_str, ip4addr_ntoa(&ipaddr), sizeof(ip_str));
	strlcpy(nm_str, ip4addr_ntoa(&netmask), sizeof(nm_str));
	strlcpy(gw_str, ip4addr_ntoa(&gw), sizeof(gw_str));
	info("Host at %s mask %s gateway %s\n", ip_str, nm_str, gw_str);

	/* Initialize */
	tcpip_init(NULL, NULL);
	info("TCP/IP initialized.\n");

	netif_add(&netif, &ipaddr, &netmask, &gw, NULL, netif_loopif_init,
		  tcpip_input);
	netif_set_link_up(&netif);
	netif_set_default(&netif);
	netif_set_up(&netif);
	info("Add netif %p\n", &netif);

	info("register server value = %u\n",
	     ipc_register_server(lwip_dispatch,
				 DEFAULT_CLIENT_REGISTER_HANDLER));

	usys_exit(0);
	return 0;
}
