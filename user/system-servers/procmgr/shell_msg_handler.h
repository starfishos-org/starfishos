#pragma once

#include <chcore/ipc.h>
#include <chcore-internal/procmgr_defs.h>

extern int shell_is_waiting;
extern int shell_cap;
extern int shell_pid;

void handle_recv_sig(ipc_msg_t *ipc_msg, struct proc_request *pr);
void handle_check_state(ipc_msg_t *ipc_msg, u64 client_badge,
			struct proc_request *pr);
void handle_get_shell_cap(ipc_msg_t *ipc_msg);
void handle_set_shell_cap(ipc_msg_t *ipc_msg, u64 client_badge);
void handle_get_terminal_cap(ipc_msg_t *ipc_msg);
void handle_set_terminal_cap(ipc_msg_t *ipc_msg);
