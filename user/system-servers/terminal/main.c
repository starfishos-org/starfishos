#include <string.h>
#include <stdnoreturn.h>
#include <pthread.h>
#include <chcore/ipc.h>
#include <chcore/type.h>
#include <chcore/syscall.h>
#include <chcore-internal/terminal_defs.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore-internal/shell_defs.h>

#include "terminal.h"
#include "util.h"

static struct terminal *terminal;
static ipc_struct_t *shell_ipc_struct;

static void ipc_server_handler(ipc_msg_t *ipc_msg, u64 client_badge)
{
	struct terminal_request *terminal_req;

	terminal_req = (struct terminal_request *)ipc_get_msg_data(ipc_msg);

	if (terminal_req->req == TERMINAL_REQ_PUT) {
		terminal_put(
			terminal, terminal_req->buffer, terminal_req->size);
	}

	ipc_return(ipc_msg, 0);
}

static noreturn void *ipc_server(void *arg)
{
	check_ret(ipc_register_server(ipc_server_handler,
				      DEFAULT_CLIENT_REGISTER_HANDLER));

	for (;;) {
		usys_yield();
	}
}

static void register_ipc_server()
{
	ipc_msg_t *ipc_msg;
	struct proc_request *proc_req;
	int ipc_server_cap;
	pthread_t tid;

	check_ret(ipc_server_cap =
			  chcore_pthread_create_services(&tid, NULL, ipc_server, NULL));

	check_ptr(ipc_msg = ipc_create_msg(
			  procmgr_ipc_struct, sizeof *proc_req, 1));
	proc_req = (struct proc_request *)ipc_get_msg_data(ipc_msg);

	proc_req->req = PROC_REQ_SET_TERMINAL_CAP;
	ipc_set_msg_cap(ipc_msg, 0, ipc_server_cap);

	check_ret(ipc_call(procmgr_ipc_struct, ipc_msg));

	ipc_destroy_msg(ipc_msg);
}

static ipc_struct_t *get_shell_ipc_struct()
{
	ipc_msg_t *ipc_msg;
	ipc_struct_t *shell_ipc_struct;
	struct proc_request *proc_req;
	u64 shell_cap;
	int ret;

	check_ptr(ipc_msg = ipc_create_msg(
			  procmgr_ipc_struct, sizeof *proc_req, 1));
	proc_req = (struct proc_request *)ipc_get_msg_data(ipc_msg);

	proc_req->req = PROC_REQ_GET_SHELL_CAP;

	ret = ipc_call(procmgr_ipc_struct, ipc_msg);
	while (ret < 0) {
		usys_yield();
		ret = ipc_call(procmgr_ipc_struct, ipc_msg);
	}

	shell_cap = ipc_get_msg_cap(ipc_msg, 0);
	ipc_destroy_msg(ipc_msg);

	shell_ipc_struct = ipc_register_client(shell_cap);
	while (shell_ipc_struct == NULL) {
		usys_yield();
		shell_ipc_struct = ipc_register_client(shell_cap);
	}

	return shell_ipc_struct;
}

static void send_to_shell(const char buffer[], size_t size)
{
	ipc_msg_t *ipc_msg;
	struct shell_req *shell_req;

	check_ptr(ipc_msg = ipc_create_msg(
			  shell_ipc_struct, sizeof *shell_req, 0));
	shell_req = (struct shell_req *)ipc_get_msg_data(ipc_msg);

	for (const char *p = buffer; p < buffer + size;
	     p += SHELL_REQ_BUFSIZE) {
		shell_req->req = SHELL_APPEND_INPUT_BUFFER;
		shell_req->size = buffer + size - p < SHELL_REQ_BUFSIZE ?
                                          buffer + size - p :
                                          SHELL_REQ_BUFSIZE;
		memcpy(shell_req->buf, buffer, shell_req->size);

		ipc_call(shell_ipc_struct, ipc_msg);
	}

	ipc_destroy_msg(ipc_msg);
}

int main(void)
{
	terminal = terminal_create(80, 24, send_to_shell);

	shell_ipc_struct = get_shell_ipc_struct();

	register_ipc_server();

	terminal_run(terminal);

	return 0;
}
