#include <errno.h>

#include "proc_node.h"
#include "procmgr_dbg.h"
#include "shell_msg_handler.h"

#define READ_ONCE(t) (*(volatile typeof((t)) *)(&(t)))
#define CTRL(A)      ((A) & 0x1f)

/* The shell main thread cap.*/
int shell_cap = -1;
int shell_pid = -1;
int shell_is_waiting = false;

/* The terminal main thread cap */
int terminal_cap = -1;

void handle_recv_sig(ipc_msg_t *ipc_msg, struct proc_request *pr)
{
	// pthread_mutex_lock(&client_proc->waiting);
	switch (pr->sig) {
	case CTRL('Z'):
		// client_proc->is_waiting = false;
		shell_is_waiting = false;
		break;
	}
	// pthread_mutex_unlock(&client_proc->waiting);
	ipc_return(ipc_msg, 0);
}

/* Check if the pointed child process has exited. */
void handle_check_state(ipc_msg_t *ipc_msg, u64 client_badge,
			struct proc_request *pr)
{
	struct proc_node *client_proc;
	int ret = 0;
	struct proc_node *child = NULL;

	/* Get client_proc */
	client_proc = get_proc_node(client_badge);
	assert(client_proc >= 0);

	pthread_mutex_lock(&client_proc->lock);
	for_each_in_list (
		child, struct proc_node, node, &client_proc->children) {
		if (child->pid == pr->pid) {
			ret = READ_ONCE(child->state);
			break;
		}
	}
	pthread_mutex_unlock(&client_proc->lock);

	/*
	 * If we can not find a child process with pid of pr->pid, it stands for
	 * that the child process is already exit. ret == 0 shows that we can not
	 * find a child process with pid of pr->pid.
	 */
	if (ret == PROC_STATE_EXIT || ret == 0)
		ret = 1;
	else
		ret = 0;

	ipc_return(ipc_msg, ret);
}

/* Send the cap of shell back that the process belongs to. */
void handle_get_shell_cap(ipc_msg_t *ipc_msg)
{
	/*
	 * We only call this function once when start a shell to prevent anyone
	 * send this ipc request to capture user's input.
	 */
	if (shell_cap == -1)
		ipc_return(ipc_msg, -1);

	ipc_msg->cap_slot_number = 1;
	ipc_set_msg_cap(ipc_msg, 0, shell_cap);
	ipc_return_with_cap(ipc_msg, 0);
}

/* Record the cap of current shell. */
void handle_set_shell_cap(ipc_msg_t *ipc_msg, u64 client_badge)
{
	struct proc_node *client_proc;

	/* Get client_proc */
	client_proc = get_proc_node(client_badge);
	assert(client_proc >= 0);

	// client_proc->shell_cap = ipc_get_msg_cap(ipc_msg, 0);
	if (shell_cap == -1) {
		shell_cap = (int)ipc_get_msg_cap(ipc_msg, 0);
		shell_pid = client_proc->pid;
		ipc_return(ipc_msg, 0);
	}
}

void handle_get_terminal_cap(ipc_msg_t *ipc_msg)
{
#if defined(CHCORE_PLAT_RASPI3) && defined(CHCORE_SERVER_GUI)
	if (terminal_cap == -1) {
		ipc_return(ipc_msg, -EAGAIN);
	}

	ipc_msg->cap_slot_number = 1;
	ipc_set_msg_cap(ipc_msg, 0, terminal_cap);
	ipc_return_with_cap(ipc_msg, 0);
#else
	ipc_return(ipc_msg, -ENOTSUP);
#endif
}

void handle_set_terminal_cap(ipc_msg_t *ipc_msg)
{
	if (terminal_cap != -1) {
		ipc_return(ipc_msg, -1);
	}

	terminal_cap = (int)ipc_get_msg_cap(ipc_msg, 0);

	ipc_return(ipc_msg, 0);
}
