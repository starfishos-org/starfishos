#include <errno.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <sys/stat.h>
#include <pthread.h>

#include <chcore/syscall.h>
#include <chcore/ipc.h>
#include <chcore/bug.h>
#include <chcore/proc.h>
#include <chcore/launcher.h>
#include <chcore/memory.h>
#include <chcore-internal/fs_defs.h>
#include <chcore-internal/shell_defs.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore/string.h>

#include "handle_input.h"
#include "job_control.h"
#include "buildin_cmd.h"

/* The buffer used to store the character from uart port and terminal. */
static char input_buf[INPUT_BUFLEN];
/* The write/read position of input_buf. */
static u32 rpos = 0, wpos = 0;
/* The mutex of input_buf */
static pthread_mutex_t input_buf_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Clear uart buffer. */
void flush_buffer(void)
{
	pthread_mutex_lock(&input_buf_mutex);
	rpos = wpos;
	pthread_mutex_unlock(&input_buf_mutex);
}

/* Get char from input_buffer. It is used by shell. */
char shell_getchar(void)
{
	char ch;

	pthread_mutex_lock(&input_buf_mutex);
	do {
		if (wpos == rpos) {
			pthread_mutex_unlock(&input_buf_mutex);
			sched_yield();
			pthread_mutex_lock(&input_buf_mutex);
		} else {
			break;
		}
	} while (1);
	ch = input_buf[rpos++];
	if (rpos == INPUT_BUFLEN) {
		rpos = 0;
	}
	pthread_mutex_unlock(&input_buf_mutex);
	return ch;
}

/* Fill character to the buffer of foreground process. */
void fill_foreground_process_stdin_buffer(void)
{
	int *fore_rpos, *fore_wpos;

	BUG_ON(foreground_buffer_addr == NULL);
	fore_rpos = (int *)foreground_pmo_addr;
	fore_wpos = fore_rpos + 1;
	pthread_mutex_lock(&input_buf_mutex);
	while (rpos != wpos
	       && (((*fore_wpos) + 1) % INPUT_BUFLEN)
			  != ((*fore_rpos) % INPUT_BUFLEN)) {
		((char *)foreground_buffer_addr)[*fore_wpos] =
			input_buf[rpos++];

		*fore_wpos = (*fore_wpos) + 1;
		if (rpos == INPUT_BUFLEN) {
			rpos = 0;
		}
		if (*fore_wpos == INPUT_BUFLEN) {
			*fore_wpos = 0;
		}
	}
	pthread_mutex_unlock(&input_buf_mutex);
}

int handle_ctrlz(char ch)
{
	struct job *new_job;
	int pos;

	/* There is no foreground thread. So it return directly. */
	if (fg_job.pid == -1) {
		return 1;
	}

	/* Add the foreground thread to the background job list. */
	pthread_mutex_lock(&job_mutex);
	new_job = (struct job *)malloc(sizeof(struct job));
	new_job->notific_cap = fg_job.notific_cap;
	new_job->pid = fg_job.pid;
	new_job->pmo_cap = fg_job.pmo_cap;
	strlcpy(new_job->job_name, fg_job.job_name, sizeof(new_job->job_name));

	fg_job.pid = -1;

	clean_jobs();
	pos = add_job(new_job->job_name,
		      new_job->pid,
		      new_job->pmo_cap,
		      new_job->notific_cap);
	pthread_mutex_unlock(&job_mutex);

	/* Send this signal to procmgr to awake the main thread of shell. */
	ipc_msg_t *msg = ipc_create_msg(
		procmgr_ipc_struct, sizeof(struct proc_request), 0);
	struct proc_request *proc_req =
		(struct proc_request *)ipc_get_msg_data(msg);
	proc_req->req = PROC_REQ_RECV_SIG;
	proc_req->sig = ch;
	ipc_call(procmgr_ipc_struct, msg);
	ipc_destroy_msg(msg);
	printf("[%d] + %d background %s\n",
	       pos + 1,
	       new_job->pid,
	       new_job->job_name);

	/* Discard the input before ctrl + z. */
	flush_buffer();
	return 1;
}

/*
 * Check the special character. It just check CTRL + Z now.
 * When shell receive a special character, it need send this msg to here to handle
 * some extra operation. For example, if shell receive a ctrl+z, procmgr need
 * awake the shell main thread.(Shell's main thread will loop on the waitpid when
 * execute a foreground process)
 */
int check_ch(char ch)
{
	int ret = 0;

	switch (ch) {
	case CTRL('Z'):
		ret = handle_ctrlz(ch);
		break;
	default:
		break;
	}
	return ret;
}

/*
 * FIXME: We just implement the uart interrupt on raspi board now and we make
 * shell to handle a part of uart interrupt. So we need to distinguish between
 * raspi platform and other platform. It is not elegant and low scalability.
 * Maybe we can add a new level to handle interrupt and receive character or try
 * other method to change this implementation.
 */
#ifdef CHCORE_PLAT_RASPI3
void handle_uart_irq_internal(void)
{
	char ch;
	unsigned int ret;

	/*
	 * FIXME:
	 * When a uart irq occur, we need read ch from uart port. But we
	 * need once memory copy for copying ch from shell buffer to
	 * process buffer. Can we have a better way to do that?
	 */
	do {
		ret = usys_getc();
		if (ret == -1)
			break;
		ch = (char)ret;
		if (check_ch(ch)) {
			break;
		}
		pthread_mutex_lock(&input_buf_mutex);
		input_buf[wpos] = ch;
		wpos++;
		if (wpos == INPUT_BUFLEN) {
			wpos = 0;
		}
		if (wpos == rpos) {
			rpos++;
			if (rpos == INPUT_BUFLEN) {
				rpos = 0;
			}
		}
		pthread_mutex_unlock(&input_buf_mutex);
	} while (1);

	// pthread_mutex_lock(&job_mutex);
	if (pthread_mutex_trylock(&job_mutex) == 0) {
		if (fg_job.pid > 0 && fg_job.notific_cap > 0) {
			fill_foreground_process_stdin_buffer();
			usys_notify(fg_job.notific_cap);
		}
		pthread_mutex_unlock(&job_mutex);
	}
	// pthread_mutex_unlock(&job_mutex);
}

void *handle_uart_irq(void *arg)
{
	int irq_cap;

#define UART_IRQ (57)
	irq_cap = usys_irq_register(UART_IRQ);
	BUG_ON(irq_cap < 0);

	while (1) {
		usys_irq_wait(irq_cap, true);
		// printf("shell irq receive:%d\n");
		handle_uart_irq_internal();
	}

	return NULL;
}

#else /* CHCORE_PLAT_RASPI3 */

/* Polling for input. */
void *other_plat_get_char(void *arg)
{
	unsigned int ret;
	char ch;

	usys_set_affinity(-1, SHELL_AFF);
	usys_yield();

	while (1) {
		do {
			ret = usys_getc();
			if (ret == -1)
				break;
			ch = (char)ret;
			if (check_ch(ch)) {
				break;
			}
			pthread_mutex_lock(&input_buf_mutex);
			input_buf[wpos] = ch;
			wpos++;
			if (wpos == INPUT_BUFLEN) {
				wpos = 0;
			}
			if (wpos == rpos) {
				rpos++;
				if (rpos == INPUT_BUFLEN) {
					rpos = 0;
				}
			}
			pthread_mutex_unlock(&input_buf_mutex);
		} while (1);

		// pthread_mutex_lock(&job_mutex);
		// if (pthread_mutex_trylock(&job_mutex) == 0) {
		// 	if (fg_job.pid > 0 && fg_job.notific_cap > 0) {
		// 		fill_foreground_process_stdin_buffer();
		// 		usys_notify(fg_job.notific_cap);
		// 	}
		// 	pthread_mutex_unlock(&job_mutex);
		// }
		sched_yield();
	}
	return NULL;
}
#endif /* CHCORE_PLAT_RASPI3 */

/* Record the infomation of process which request the user's input. */
void set_process_info(int notific_cap, int buffer_cap, pid_t pid)
{
	int i = 0, j = 0;
	int total_job;

	// printf("pid:%d %d\n", pid, fg_job.pid);
	pthread_mutex_lock(&job_mutex);
	if (pid == fg_job.pid) {
		fg_job.notific_cap = notific_cap;
		fg_job.pmo_cap = buffer_cap;
		foreground_pmo_addr = (void *)chcore_auto_map_pmo(
			buffer_cap, PAGE_SIZE, PROT_READ | PROT_WRITE);
		BUG_ON(foreground_pmo_addr == NULL);
		foreground_buffer_addr = foreground_pmo_addr + sizeof(u32) * 2;
	} else {
		total_job = job_count;
		for (i = 0; i < total_job;) {
			/*
			 * Make an assumption that the thread which gets input
			 * have not exited.
			 */
			if (bg_jobs[j] != NULL) {
				if (bg_jobs[j]->pid == pid) {
					bg_jobs[j]->pmo_cap = buffer_cap;
					bg_jobs[j]->notific_cap = notific_cap;
					break;
				}
				i++;
			}
			j++;
		}
		/*
		 * There is no job that can match the client. So it is not the
		 * child process of shell.
		 */
		goto out;
	}

	if (rpos != wpos) {
		fill_foreground_process_stdin_buffer();
	}
out:
	pthread_mutex_unlock(&job_mutex);
}

/* append to the input buffer (called by the terminal) */
void append_input_buffer(const char buf[], size_t size)
{
	for (size_t i = 0; i < size; i++) {
		if (check_ch(buf[i])) {
			continue;
		}

		pthread_mutex_lock(&input_buf_mutex);
		input_buf[wpos] = buf[i];
		wpos++;
		if (wpos == INPUT_BUFLEN) {
			wpos = 0;
		}
		if (wpos == rpos) {
			rpos++;
			if (rpos == INPUT_BUFLEN) {
				rpos = 0;
			}
		}
		pthread_mutex_unlock(&input_buf_mutex);
	}

	if (pthread_mutex_trylock(&job_mutex) == 0) {
		if (fg_job.pid > 0 && fg_job.notific_cap > 0) {
			fill_foreground_process_stdin_buffer();
			usys_notify(fg_job.notific_cap);
		}
		pthread_mutex_unlock(&job_mutex);
	}
}

/*
 * When a process want to read character from uart buffer, it must send it's
 * necessary information to shell such as notification cap and buffer pmo cap.
 * So when an interrupt comes in, shell can fill character to foreground process's
 * buffer and awake it.
 * 
 * The terminal sends keyboard input to the shell.
 */
void shell_dispatch(ipc_msg_t *ipc_msg, u64 client_badge)
{
	struct shell_req *req = (struct shell_req *)ipc_get_msg_data(ipc_msg);

	switch (req->req) {
	case SHELL_SET_PROCESS_INFO:
		if (ipc_msg->cap_slot_number != 2) {
			printf("Request para error!\n");
			break;
		}
		set_process_info((int)ipc_get_msg_cap(ipc_msg, 0),
				 (int)ipc_get_msg_cap(ipc_msg, 1),
				 req->pid);
		break;
	case SHELL_APPEND_INPUT_BUFFER: {
		size_t append_size = req->size;
		if (append_size > sizeof(req->buf))
			append_size = sizeof(req->buf);
		append_input_buffer(req->buf, append_size);
		break;
	}
	default:
		printf("wrong request\n");
		break;
	}
	ipc_return(ipc_msg, 0);
}

void *shell_server(void *arg)
{
	int ret = ipc_register_server(shell_dispatch, register_cb_single);
	BUG_ON(ret < 0);

	/*
	 * TODO: we should suspend the thread that register a ipc server. We should
	 * use a more elegant way to do that.
	 */
	usys_wait(usys_create_notifc(), true, NULL);
	return NULL;
}

/*
 * FIXME: The child processes of shell need know the cap of shell server. At the
 * begining, I want to send the cap by chcore_new_process, and add the cap to
 * child processes when call the launch_process_with_pmo_caps function. But it
 * will trigger a bug before the process start. So I maintain a field in proc_node
 * struct to record the cap of shell that the process belongs to. Maybe we can
 * send the cap by launch_process_with_pmo_caps directly in the future.
 */
void send_cap_to_procmgr(int cap)
{
	ipc_msg_t *msg;
	struct proc_request *proc_req;

	msg = ipc_create_msg(
		procmgr_ipc_struct, sizeof(struct proc_request), 1);
	proc_req = (struct proc_request *)ipc_get_msg_data(msg);

	msg->cap_slot_number = 1;
	ipc_set_msg_cap(msg, 0, cap);
	proc_req->req = PROC_REQ_SET_SHELL_CAP;
	ipc_call(procmgr_ipc_struct, msg);

	ipc_destroy_msg(msg);
}
