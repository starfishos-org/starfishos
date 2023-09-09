#include <stdio.h>
#include <sys/types.h>
#include <chcore/ipc.h>
#include <string.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore/syscall.h>
#include "fd.h"
#include "pthread_impl.h"

extern void chcore_reinitialize_stdio();
extern int reconnect_to_system_servers(u64 new_fs_cap, u64 new_lwip_cap, u64 new_procmgr_cap);
extern ipc_struct_t *procmgr_ipc_struct;

struct clone_cap_group_args {
	u64 child_badge;
	u64 child_pcid;
	u64 child_mt_cap;
	u64 fs_server_cap;
	u64 lwip_server_cap;
	u64 procmgr_server_cap;
	u64 parent_badge;
};

extern int chcore_pid;
int chcore_do_fork()
{
	pid_t pid;
	int ret;
	ipc_msg_t *ipc_msg;
	struct proc_request pr;
	struct fsm_request fsmr;
	u64 child_badge;
	u64 pcid;
	struct clone_cap_group_args args;

	/* Construct ipc_msg */
	ipc_msg = ipc_create_msg(procmgr_ipc_struct,
				 sizeof(struct proc_request), 0);
	pr.req = PROC_REQ_FORK;
	memcpy(ipc_get_msg_data(ipc_msg), &pr, sizeof(struct proc_request));

	/* Get child badge and child pid */
	child_badge = ipc_call(procmgr_ipc_struct, ipc_msg);
	pid = ((struct proc_request *)ipc_get_msg_data(ipc_msg))->pid;
	pcid = ((struct proc_request *)ipc_get_msg_data(ipc_msg))->pcid;
	ipc_destroy_msg(ipc_msg);

	args.child_badge = child_badge;
	args.child_pcid = pcid;
	args.fs_server_cap = fsm_server_cap;
	args.lwip_server_cap = lwip_server_cap;
	args.procmgr_server_cap = procmgr_server_cap;
	if ((ret = usys_clone_cap_group((u64)&args)) < 0) {
		// fork failed
		return ret;
	} else if (ret > 0) {
		// parent
		return pid;
	} else {
		// child
		/* reinitialize stdio */
		chcore_reinitialize_stdio();
		/* set pid */
		chcore_pid = pid;
		/* reconnect system server */
		reconnect_to_system_servers(args.fs_server_cap,
					    args.lwip_server_cap,
					    args.procmgr_server_cap);
		ipc_msg = ipc_create_msg(procmgr_ipc_struct,
				 sizeof(struct proc_request), 2);
		pr.req = PROC_CHILD_FINISH_FORK;
		memcpy(ipc_get_msg_data(ipc_msg), &pr, sizeof(struct proc_request));
		ipc_set_msg_cap(ipc_msg, 0, SELF_CAP);
		ipc_set_msg_cap(ipc_msg, 1, args.child_mt_cap);
		ipc_call(procmgr_ipc_struct, ipc_msg);
		ipc_destroy_msg(ipc_msg);
		{
			/* reinitialize libc metadata */
			pthread_t self = __pthread_self();
			self->robust_list.off = 0;
			self->robust_list.pending = 0;
			self->next = self->prev = self;
			__thread_list_lock = 0;
			libc.threads_minus_1 = 0;
		}
		ipc_msg = ipc_create_msg(fsm_ipc_struct,
				 sizeof(struct fs_request), 2);
		fsmr.req = FSM_CHILD_FINISH_FORK;
		fsmr.parentBagde = args.parent_badge;
		memcpy(ipc_get_msg_data(ipc_msg), &fsmr, sizeof(struct fsm_request));
		ipc_call(fsm_ipc_struct, ipc_msg);
		ipc_destroy_msg(ipc_msg);

		return 0;
	}
}
weak_alias(chcore_do_fork, fork);
