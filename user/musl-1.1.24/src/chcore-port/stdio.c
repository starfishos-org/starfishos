/* File operation towards STDIN, STDOUT and STDERR */

#include "fd.h"
#include <assert.h>
#include <errno.h>
#include <syscall_arch.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <chcore/memory.h>
#include <termios.h>
#include <chcore-internal/shell_defs.h>
#include <chcore-internal/terminal_defs.h>
#include <chcore-internal/procmgr_defs.h>
#include <unistd.h>
#include <fcntl.h>


/*
 * We create a pmo to store the user's input. The size of pmo is 4096 byte but
 * the size of input buffer is 4088. Since we use the first two four-byte for
 * store the position of write/read pointer.
 */
#define BUFLEN (PAGE_SIZE - sizeof(int) * 2)

static int input_notific_cap = -1;
static int stdin_buff_cap = -1;
static void *stdin_buffer = NULL;
static void *stdin_pmo_addr = NULL;
static int *rpos, *wpos;
extern int procmgr_server_cap;
static int shell_server_cap = -1;

static ipc_struct_t *shell_msg_struct = NULL;
static int pid = -1;

static ipc_struct_t *terminal_ipc_struct = NULL;
static int has_terminal = 1;

void chcore_reinitialize_stdio()
{
	terminal_ipc_struct = NULL;
	shell_msg_struct = NULL;
}

static int get_one_char()
{
	int ch;

	ipc_msg_t *shell_msg;
	ipc_msg_t *procmgr_msg;
	struct proc_request *proc_req;
	struct shell_req *req;

	if (input_notific_cap == -1) {
		input_notific_cap = usys_create_notifc();
		BUG_ON(input_notific_cap < 0);
	}

	if (stdin_buff_cap == -1) {
		stdin_buff_cap = usys_create_pmo(PAGE_SIZE, PMO_DATA);
		BUG_ON(stdin_buff_cap < 0);
		stdin_pmo_addr = (void *)chcore_auto_map_pmo(
			stdin_buff_cap, PAGE_SIZE, PROT_READ | PROT_WRITE);
		stdin_buffer = (char *)stdin_pmo_addr + sizeof(int) * 2;
		/*
		 * We use the first 8 bytes of stdin_buffer to standard for the
		 * write/read pointer.
		 */
		rpos = (int *)stdin_pmo_addr;
		wpos = rpos + 1;
		*rpos = 0;
		*wpos = 0;
	}

	if (pid == -1) {
		pid = getpid();
	}

	if (shell_msg_struct == NULL) {
		procmgr_msg = ipc_create_msg(
			procmgr_ipc_struct, sizeof(struct proc_request), 1);
		proc_req = (struct proc_request *)ipc_get_msg_data(procmgr_msg);

		proc_req->req = PROC_REQ_GET_SHELL_CAP;
		ipc_call(procmgr_ipc_struct, procmgr_msg);
		BUG_ON(procmgr_msg->cap_slot_number == 0);

		shell_server_cap = ipc_get_msg_cap(procmgr_msg, 0);
		BUG_ON(shell_server_cap < 0);
		ipc_destroy_msg(procmgr_msg);

		shell_msg_struct = ipc_register_client(shell_server_cap);
		BUG_ON(shell_msg_struct == NULL);

		shell_msg = ipc_create_msg(
			shell_msg_struct, sizeof(struct shell_req), 2);
		req = (struct shell_req *)ipc_get_msg_data(shell_msg);
		req->pid = pid;
		req->req = SHELL_SET_PROCESS_INFO;
		ipc_set_msg_cap(shell_msg, 0, input_notific_cap);
		ipc_set_msg_cap(shell_msg, 1, stdin_buff_cap);
		// printf("send msg\n");
		ipc_call(shell_msg_struct, shell_msg);
		ipc_destroy_msg(shell_msg);
	}

	while (*rpos == *wpos) {
		usys_wait(input_notific_cap, 1, NULL);
	}

	ch = ((char *)stdin_buffer)[*rpos];
	*rpos = (*rpos) + 1;
	if (*rpos == BUFLEN) {
		*rpos = 0;
	}

	return ch;
}

static void put(char buffer[], unsigned size)
{
	ipc_msg_t *terminal_msg;
	ipc_msg_t *procmgr_msg;
	struct proc_request *proc_req;
	struct terminal_request *terminal_req;
	int terminal_server_cap, ret;
	unsigned i;

	for (i = 0; i < size; i++) {
		chcore_syscall1(CHCORE_SYS_putc, buffer[i]);
	}

	/*
	 * The processes that have a invalid procmgr_server_cap can not communication
	 * with procmgr.
	 */
	if (procmgr_server_cap <= 0) {
		return;
	}

	if (has_terminal && terminal_ipc_struct == NULL) {
		procmgr_msg =
			ipc_create_msg(procmgr_ipc_struct, sizeof *proc_req, 1);
		proc_req = (struct proc_request *)ipc_get_msg_data(procmgr_msg);

		proc_req->req = PROC_REQ_GET_TERMINAL_CAP;

		ret = ipc_call(procmgr_ipc_struct, procmgr_msg);
		if (ret < 0) {
			if (ret != -EAGAIN) {
				has_terminal = 0;
			}

			ipc_destroy_msg(procmgr_msg);
			return;
		}

		terminal_server_cap = ipc_get_msg_cap(procmgr_msg, 0);

		terminal_ipc_struct = ipc_register_client(terminal_server_cap);

		ipc_destroy_msg(procmgr_msg);
	}

	if (terminal_ipc_struct != NULL) {
		terminal_msg = ipc_create_msg(
			terminal_ipc_struct, sizeof *terminal_req, 0);
		terminal_req = (struct terminal_request *)ipc_get_msg_data(
			terminal_msg);

		terminal_req->req = TERMINAL_REQ_PUT;
		memcpy(terminal_req->buffer, buffer, size);
		terminal_req->size = size;

		ipc_call(terminal_ipc_struct, terminal_msg);

		ipc_destroy_msg(terminal_msg);
	}
}

#define MAX_LINE_SIZE 4095

int chcore_stdio_fcntl(int fd, int cmd, int arg)
{
	int new_fd, ret = 0;

	switch (cmd) {
	case F_DUPFD_CLOEXEC:
	case F_DUPFD: {
		new_fd = dup_fd_content(fd, arg);
		return new_fd;
	}
	default:
		return -EINVAL;
	}
	return -1;
}

/* STDIN */
static int chcore_stdin_read(int fd, void *buf, size_t count)
{
	struct fd_desc *fdesc = fd_dic[fd];
	assert(fdesc);

	struct termios *ts = &fdesc->termios;
	char *char_buf = (char *)buf;
	size_t n = 0;
	int ch;
	bool canon_mode = ts->c_lflag & ICANON;

	/*
	 * Read from stdin according to termios.
	 * For simplicity, we don't support every flag.
	 */
	while (n < MIN(count, MAX_LINE_SIZE)) {
		ch = get_one_char();
		if (ch == '\r') {
			if (ts->c_iflag & IGNCR) {
				/* ignore CR */
				continue;
			} else if (ts->c_iflag & ICRNL) {
				/* tranlate CR to NL */
				ch = '\n';
			}
		} else if (ch == '\n' && (ts->c_iflag & INLCR)) {
			ch = '\r';
		} else if (ch == '\x7f' /* DELETE */) {
			/* FIXME: currently we see DELETE as BACKSPACE */
			ch = '\b';
		}

		if (ch == '\b' && canon_mode) {
			if (n > 0) {
				if (ts->c_lflag & ECHOE) {
					putchar('\b');
					putchar(' ');
					putchar('\b');
					fflush(stdout);
				} else if (ts->c_lflag & ECHO) {
					putchar('\b');
					fflush(stdout);
				}
				n--; /* remove one char from buffer */
			} else {
				/* alert that no char to remove */
				putchar('\a');
				fflush(stdout);
			}
			continue;
		} else if (ch != '\b' && ts->c_lflag & ECHO) {
			putchar(ch);
			fflush(stdout);
		}

		/* add ch to buffer */
		char_buf[n++] = ch;

		if (!canon_mode && n >= ts->c_cc[VMIN]) {
			/* VMIN == 0 and VTIME are ignored */
			break;
		} else if (canon_mode && ch == '\n') {
			/* canonical mode + new line */
			break;
		}
	}

	char_buf[n] = '\0';
	return n;
}

static int chcore_stdin_write(int fd, void *buf, size_t count)
{
	return -EINVAL;
}

static int chcore_stdin_close(int fd)
{
	free_fd(fd);
	return 0;
}

static int chcore_stdio_ioctl(int fd, unsigned long request, void *arg)
{
	/* A fake window size */
	if (request == TIOCGWINSZ) {
		struct winsize *ws;

		ws = (struct winsize *)arg;
		ws->ws_row = 10;
		ws->ws_col = 80;
		ws->ws_xpixel = 0;
		ws->ws_ypixel = 0;
		return 0;
	}
	struct fd_desc *fdesc = fd_dic[fd];
	assert(fdesc);
	switch (request) {
	case TCGETS: {
		struct termios *t = (struct termios *)arg;
		*t = fdesc->termios;
		return 0;
	}
	case TCSETS:
	case TCSETSW:
	case TCSETSF: {
		struct termios *t = (struct termios *)arg;
		fdesc->termios = *t;
		return 0;
	}
	}
	warn("Unsupported ioctl fd=%d, cmd=0x%lx, arg=0x%lx\n",
	     fd,
	     request,
	     arg);
	return 0;
}

struct fd_ops stdin_ops = {
	.read = chcore_stdin_read,
	.write = chcore_stdin_write,
	.close = chcore_stdin_close,
	.poll = NULL,
	.ioctl = chcore_stdio_ioctl,
	.fcntl = chcore_stdio_fcntl,
};

/* STDOUT */
static int chcore_stdout_read(int fd, void *buf, size_t count)
{
	return -EINVAL;
}

static int chcore_stdout_write(int fd, void *buf, size_t count)
{
	/* TODO: stdout should also follow termios flags */
	char buffer[TERMINAL_REQ_BUFSIZE];
	size_t size = 0;

	for (char *p = buf; p < (char *)buf + count; p++) {
		if (size + 2 > TERMINAL_REQ_BUFSIZE) {
			put(buffer, size);
			size = 0;
		}

		if (*p == '\n') {
			buffer[size++] = '\r';
		}
		buffer[size++] = *p;
	}

	if (size > 0) {
		put(buffer, size);
	}

	return count;
}

static int chcore_stdout_close(int fd)
{
	free_fd(fd);
	return 0;
}

struct fd_ops stdout_ops = {
	.read = chcore_stdout_read,
	.write = chcore_stdout_write,
	.close = chcore_stdout_close,
	.poll = NULL,
	.ioctl = chcore_stdio_ioctl,
	.fcntl = chcore_stdio_fcntl,
};

/* STDERR */
static int chcore_stderr_read(int fd, void *buf, size_t count)
{
	return -EINVAL;
}

static int chcore_stderr_write(int fd, void *buf, size_t count)
{
	return chcore_stdout_write(fd, buf, count);
}

static int chcore_stderr_close(int fd)
{
	free_fd(fd);
	return 0;
}

struct fd_ops stderr_ops = {
	.read = chcore_stderr_read,
	.write = chcore_stderr_write,
	.close = chcore_stderr_close,
	.poll = NULL,
	.ioctl = chcore_stdio_ioctl,
	.fcntl = chcore_stdio_fcntl,
};
