#pragma once

#include <chcore/ipc.h>
#include <chcore-internal/procmgr_defs.h>

struct proc_node *procmgr_launch_process(int input_argc, char **input_argv,
					 char *name, bool if_has_parent,
					 u64 parent_badge, bool is_cross_machine);
struct proc_node *procmgr_launch_basic_server(int input_argc, char **input_argv,
					 char *name, bool if_has_parent,
					 u64 parent_badge);

/*
 * There three kinds of servers in ChCore for now.
 *  1. servers booted before procmgr like cxlfs, fsm and lwip
 *  2. servers which will be booted once procmgr is up like usb_devmgr
 *  3. servers which will be booted lazily like shmmgr, hdmi, sd4, etc.
 * 
 * boot_secondary_servers() is in charge of booting the second kind of servers.
 */
void set_cxlfs_cap(int cap);
void boot_secondary_servers(void);

/* Initialize sys_servers and sys_server_locks */
void init_srvmgr(void);
void handle_get_server_cap(ipc_msg_t *ipc_msg, struct proc_request *pr);
