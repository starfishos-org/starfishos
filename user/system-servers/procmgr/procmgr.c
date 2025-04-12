#define _GNU_SOURCE
#include <chcore/ipc.h>
#include <chcore/launcher.h>
#include <chcore/proc.h>
#include <chcore/syscall.h>
#include <chcore-internal/procmgr_defs.h>
#include <pthread.h>
#include <string.h>
#include <malloc.h>
#include <sys/mman.h>
#include <chcore/launch_kern.h>
#include <sched.h>

#include "proc_node.h"
#include "procmgr_dbg.h"
#include "srvmgr.h"
#include "shell_msg_handler.h"

/* fsm_server_cap in current process; can be copied to others */
extern int fsm_server_cap;
/* lwip_server_cap in current process; can be copied to others */
extern int lwip_server_cap;

#define READ_ONCE(t) (*(volatile typeof((t)) *)(&(t)))

extern char __binary_fsm_elf_start;
extern char __binary_fsm_elf_size;

static char *str_join(char *delimiter, char *strings[], int n)
{
        char buf[256];
        size_t size = 256 - 1; /* 1 for the trailing \0. */
        size_t dlen = strlen(delimiter);
        char *dst = buf;
        int i;
        for (i = 0; i < n; ++i) {
                size_t l = strlen(strings[i]);
                if (i > 0)
                        l += dlen;
                if (l > size) {
                        printf("str_join string buffer overflow\n");
                        break;
                }
                if (i > 0) {
                        strlcpy(dst, delimiter, size);
                        strlcpy(dst + dlen, strings[i], size - dlen);
                } else {
                        strlcpy(dst, strings[i], size);
                }
                dst += l;
                size -= l;
        }
        *dst = 0;
        return strdup(buf);
}

static void handle_newproc(ipc_msg_t *ipc_msg, u64 client_badge,
                           struct proc_request *pr)
{
        int input_argc = pr->argc;
        char *input_argv[PROC_REQ_ARGC_MAX];
        struct proc_node *proc_node;
        int is_cross_machine = pr->is_cross_machine;

        /* Translate to argv pointers from argv offsets. */
        for (int i = 0; i < input_argc; ++i)
                input_argv[i] = &pr->text[pr->argv[i]];

        proc_node = procmgr_launch_process(
                input_argc,
                input_argv,
                str_join(" ", &input_argv[0], input_argc),
                true,
                client_badge,
                is_cross_machine);
        if (proc_node == NULL) {
                ipc_return(ipc_msg, -1);
        } else {
                ipc_return(ipc_msg, proc_node->pid);
        }
}

static void handle_wait(ipc_msg_t *ipc_msg, u64 client_badge,
                        struct proc_request *pr)
{
        struct proc_node *client_proc;
        struct proc_node *child = NULL;
        pid_t ret_pid;

        /* Get client_proc */
        client_proc = get_proc_node(client_badge);
        assert(client_proc);

        /*
         * May be we use the child thread state to identify whether we need to
         * wait child process in the future.
         */
        if (client_proc->pid == shell_pid) {
                shell_is_waiting = true;
        }

        while (1) {
                /* Use a lock to synsynchronize this function and del_proc_node
                 */
                pthread_mutex_lock(&client_proc->lock);
                /*
                 * FIXME:
                 * pr->pid == -1 means waiting for any child process.
                 */
                for_each_in_list (
                        child, struct proc_node, node, &client_proc->children) {
                        if (child->pid == pr->pid || pr->pid == -1)
                                break;
                }

                if (!child || (child->pid != pr->pid && pr->pid != -1)) {
                        /* wrong pid */
                        /*
                         * TODO: if a process has already exited,
                         * waitpid cannot get its exit value for now.
                         */
                        pthread_mutex_unlock(&client_proc->lock);
                        ipc_return(ipc_msg, -ESRCH);
                }

                if (client_proc->pid == shell_pid
                    && (!READ_ONCE(shell_is_waiting))) {
                        pthread_mutex_unlock(&client_proc->lock);
                        ipc_return(ipc_msg, -EINTR);
                }

                /* Found. */
                debug("Found process with pid=%d proc=%p\n", pr->pid, child);

                if (READ_ONCE(child->state) == PROC_STATE_EXIT) {
                        /*
                         * The exit status has been set but the node
                         * has not been removed from its parent process`s
                         * child list.
                         */
                        debug("Process (pid=%d, proc=%p) exits with %d\n",
                              pr->pid,
                              child,
                              child->exitstatus);
                        pr->exitstatus = child->exitstatus;
                        ret_pid = child->pid;
                        /*
                         * Delete the child node from the children list of
                         * parent. and recycle the proc node of child.
                         */
                        pthread_mutex_lock(&recycle_lock);
                        list_del(&child->node);
                        free_proc_node_resource(child);
                        free(child);
                        pthread_mutex_unlock(&recycle_lock);
                        pthread_mutex_unlock(&client_proc->lock);
                        ipc_return(ipc_msg, ret_pid);
                } else {
                        /* Child process has not exited yet, try again later */
                        pthread_mutex_unlock(&client_proc->lock);
                        usys_yield();
                }
        }
}

static void handle_get_thread_cap(ipc_msg_t *ipc_msg, u64 client_badge,
                                  struct proc_request *pr)
{
        struct proc_node *client_proc;
        struct proc_node *child = NULL;

        /* Get client_proc */
        client_proc = get_proc_node(client_badge);
        assert(client_proc);

        for_each_in_list (
                child, struct proc_node, node, &client_proc->children) {
                if (child->pid == pr->pid)
                        break;
        }
        if (!child || (child->pid != pr->pid && pr->pid != -1))
                ipc_return(ipc_msg, -ENONET);

        /* Found. */
        debug("Found process with pid=%d proc=%p\n", pr->pid, child);

        /*
         * Set the main-thread cap in the ipc_msg and
         * the following ipc_return_with_cap will transfer the cap.
         */
        ipc_msg->cap_slot_number = 1;
        ipc_set_msg_cap(ipc_msg, 0, child->proc_mt_cap);
        ipc_return_with_cap(ipc_msg, 0);
}

static void handle_fork(ipc_msg_t *ipc_msg, u64 client_badge)
{
        struct proc_node *client_proc;
        struct proc_node *child = NULL;
        struct proc_request *pr;
        char *name;

        /* Get client_proc */
        client_proc = get_proc_node(client_badge);
        assert(client_proc);

        /*
         * Create a new proc node for the child.
         * Later, the child process will ipc_call
         * to complete the missing info.
         *
         */
        name = malloc(strlen(client_proc->name));
        strcpy(name, client_proc->name);
        child = new_proc_node(client_proc, name);
        pr = (struct proc_request *)ipc_get_msg_data(ipc_msg);
        pr->pid = child->pid;
        pr->pcid = child->pcid;

        ipc_return(ipc_msg, child->badge);
}

/*
 * The forked child process will invoke this function to complete
 * the missing info (cap_group_cap and main_thread_cap) which will
 * be needed when recylcing this process.
 */
static void handle_finish_fork(ipc_msg_t *ipc_msg, u64 client_badge)
{
        struct proc_node *client_proc;
        u64 cap_group_cap, mt_cap;

        /* Get client_proc */
        client_proc = get_proc_node(client_badge);
        assert(client_proc);

        cap_group_cap = ipc_get_msg_cap(ipc_msg, 0);
        mt_cap = ipc_get_msg_cap(ipc_msg, 1);
        client_proc->proc_cap = cap_group_cap;
        client_proc->proc_mt_cap = mt_cap;

        ipc_return(ipc_msg, 0);
}

static int init_procmgr(void)
{
        /* Init proc_node manager */
        init_proc_node_mgr();

        /* Init server manager */
        init_srvmgr();
        return 0;
}

void procmgr_dispatch(ipc_msg_t *ipc_msg, u64 client_badge)
{
        struct proc_request *pr;

        debug("new request from client_badge: 0x%lx\n", client_badge);

        if (ipc_msg->data_len < 4) {
                error("FSM: no operation num\n");
                usys_exit(-1);
        }

        pr = (struct proc_request *)ipc_get_msg_data(ipc_msg);
        debug("req: %d\n", pr->req);

        switch (pr->req) {
        case PROC_REQ_NEWPROC:
                handle_newproc(ipc_msg, client_badge, pr);
                break;
        case PROC_REQ_WAIT:
                handle_wait(ipc_msg, client_badge, pr);
                break;
        case PROC_REQ_GET_MT_CAP:
                handle_get_thread_cap(ipc_msg, client_badge, pr);
                break;
        /*
         * Get server_cap by server_id.
         * Skip get_proc_node for the following IPC REQ.
         * This is because FSM will invoke this IPC but it is not
         * booted by procmgr and @client_proc is not required in the IPC.
         */
        case PROC_REQ_GET_SERVER_CAP:
                handle_get_server_cap(ipc_msg, pr);
                break;
        case PROC_REQ_RECV_SIG:
                handle_recv_sig(ipc_msg, pr);
                break;
        case PROC_REQ_CHECK_STATE:
                handle_check_state(ipc_msg, client_badge, pr);
                break;
        case PROC_REQ_GET_SHELL_CAP:
                handle_get_shell_cap(ipc_msg);
                break;
        case PROC_REQ_SET_SHELL_CAP:
                handle_set_shell_cap(ipc_msg, client_badge);
                break;
        case PROC_REQ_GET_TERMINAL_CAP:
                handle_get_terminal_cap(ipc_msg);
                break;
        case PROC_REQ_SET_TERMINAL_CAP:
                handle_set_terminal_cap(ipc_msg);
                break;
        case PROC_REQ_FORK:
                handle_fork(ipc_msg, client_badge);
                break;
        case PROC_CHILD_FINISH_FORK:
                handle_finish_fork(ipc_msg, client_badge);
                break;
        default:
                error("Invalid request type %d!\n", pr->req);
                /* Client should check if the return value is correct */
                ipc_return(ipc_msg, -EBADRQC);
                break;
        }
}

void *recycle_routine(void *arg);

/*
 * Procmgr is the first user process and there is no system services now.
 * So, override the default libc_connect_services.
 */
void libc_connect_services(char *envp[])
{
        procmgr_ipc_struct->conn_cap = 0;
        procmgr_ipc_struct->server_id = PROC_MANAGER;
        return;
}

extern int __procmgr_server_cap;

void boot_default_servers(void)
{
        char *srv_path;
        int tmpfs_cap;
        struct proc_node *proc_node;

        /* Do not modify the order of creating system servers */
        printf("User Init: booting fs server (FSMGR and real FS) \n");

        srv_path = "/tmpfs.srv";
        proc_node = procmgr_launch_basic_server(
                1, &srv_path, "tmpfs", true, INIT_BADGE);
        tmpfs_cap = proc_node->proc_mt_cap;
        /*
         * We set the cap of tmpfs before starting fsm to ensure that tmpfs is
         * available after fsm is started.
         */
        set_tmpfs_cap(tmpfs_cap);

        /* FSM gets badge 2 and tmpfs uses the fixed badge (10) for it */
        srv_path = "/fsm.srv";
        proc_node = procmgr_launch_basic_server(
                1, &srv_path, "fsm", true, INIT_BADGE);
        fsm_server_cap = proc_node->proc_mt_cap;
        fsm_ipc_struct->server_id = FS_MANAGER;

        printf("User Init: booting network server\n");
        /* Pass the FS cap to NET since it may need to read some config files */
        /* Net gets badge 3 */
        srv_path = "/lwip.srv";
        proc_node =
                procmgr_launch_process(1, &srv_path, "lwip", true, INIT_BADGE, false);
        lwip_server_cap = proc_node->proc_mt_cap;
}

void *handler_thread_routine(void *arg)
{
        int ret;
        ret = ipc_register_server(procmgr_dispatch,
                                  DEFAULT_CLIENT_REGISTER_HANDLER);
        printf("[procmgr] register server value = %d\n", ret);
        // while(1) {
        // 	sched_yield();
        // };
        // FN: wait here so this thread will not pin in sched queue
        usys_wait(usys_create_notifc(), 1, NULL);
        return NULL;
}

void boot_default_apps(void)
{
        char *shell_argv = "chcore_shell.bin";

#define CONFIG_MACHINE_ARM 0
#if CONFIG_MACHINE_ARM == 1
        /*
         * Because the machine arm occupy the serial port, so ChCore can not
         * receive keybord and send characters to screen from uart now. We start
         * the arm server at here for test. It can be deleted when hdmi and
         * keyboard driver are ready.
         */
        char *args[1];
        args[0] = "/machine_arm.bin";

        procmgr_launch_process(1, args, "machine_arm", true, INIT_BADGE, false);
#endif

        /* Start shell. */
        procmgr_launch_process(
                1, &shell_argv, "chcore-shell", true, INIT_BADGE, false);
#if defined(CHCORE_PLAT_RASPI3) && defined(CHCORE_SERVER_GUI)
        char *terminal_argv = "terminal.bin";
        procmgr_launch_process(1, &terminal_argv, "terminal", true, INIT_BADGE, false);
#endif
}

int main(int argc, char *argv[], char *envp[])
{
        int ret = 0;
        pthread_t recycle_thread;
        pthread_t procmgr_handler_tid;

        /*
         * Prepare the recycle thread.
         * TODO (minor): what if this becomes a bottleneck?
         */
        pthread_create(&recycle_thread, NULL, recycle_routine, NULL);

        init_procmgr();
        ret = chcore_pthread_create_services(
                &procmgr_handler_tid, NULL, handler_thread_routine, NULL);

        __procmgr_server_cap = ret;

        init_root_proc_node();

        boot_default_servers();

        boot_default_apps();

        /* Boot some configurable servers which should be booted lazily */
        boot_secondary_servers();

        usys_wait(usys_create_notifc(), 1, NULL);
        return 0;
}
