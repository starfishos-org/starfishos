#include <chcore/services.h>

#ifdef __cplusplus
extern "C" {
#endif

ipc_struct_t *chcore_conn_srv(enum CONFIGURABLE_SERVER srv_id)
{
	extern ipc_struct_t *procmgr_ipc_struct;
	ipc_msg_t *ipc_msg;
	struct proc_request *pr;
	int srv_cap;
	s64 ret;

	/* Get server cap */
	ipc_msg = ipc_create_msg(procmgr_ipc_struct,
		sizeof(struct proc_request), 0);
	pr = (struct proc_request *)ipc_get_msg_data(ipc_msg);
	pr->req = PROC_REQ_GET_SERVER_CAP;
	pr->server_id = srv_id;

	ret = ipc_call(procmgr_ipc_struct, ipc_msg);
	if (ret < 0) {
		ipc_destroy_msg(ipc_msg);
		return NULL;
	}
	srv_cap = ipc_get_msg_cap(ipc_msg, 0);
	ipc_destroy_msg(ipc_msg);

	/* Register client */
	return ipc_register_client(srv_cap);
}

#ifdef __cplusplus
}
#endif