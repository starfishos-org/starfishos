#include <chcore/ipc.h>
#include <chcore/launcher.h>
#include <chcore/proc.h>
#include <chcore/syscall.h>
#include <chcore-internal/procmgr_defs.h>
#include <chcore/uapi/thread.h>
#include <string.h>

#include "proc_node.h"
#include "procmgr_dbg.h"
#include "srvmgr.h"

/*
 * Note: This is not an isolated server. It is still a part of procmgr and resides
 * in the same address space. We just take the code related to server management apart
 * from procmgr.c and put it here. Code related to server management like booting a
 * configurable server or get some info about a server should be put in this file from
 * now on.
 */

/* Global root system server caps */
extern int fsm_server_cap;
extern int lwip_server_cap;

static int nr_caps = 0;

/*
 * NOTE: We do not initialize the data structure in procmgr to prevent procmgr
 * from registering client of itself. Otherwise, it will lead to memory leak when
 * procmgr calls printf. If put() do not send IPC msg, it can be optimized.
 */
int __procmgr_server_cap;

/*
 * To read elf atomically.
 * FIXME: buggy impl now
 */
static pthread_mutex_t read_elf_lock;

/* Array of the caps of the system servers */
static int sys_servers[CONFIG_SERVER_MAX];
/* To launch system server atomically */
static pthread_mutex_t sys_server_locks[CONFIG_SERVER_MAX];

/* For booting system servers. */
static void boot_server(char *srv_name, char *srv_path, int *srv_cap_p)
{
	char *input_argv[PROC_REQ_ARGC_MAX];
	input_argv[0] = srv_path;
	*srv_cap_p = procmgr_launch_process(1, input_argv, srv_name, false, 0, false)
			     ->proc_mt_cap;
}

void set_tmpfs_cap(int cap)
{
	sys_servers[SERVER_TMPFS] = cap;
}

extern char __binary_fsm_elf_start;
extern char __binary_fsm_elf_size;

extern char __binary_tmpfs_elf_start;
extern char __binary_tmpfs_elf_size;

/* Return proc_node of new process */
static struct proc_node *do_launch_process(int input_argc, char **input_argv,
                                           char *name, bool if_has_parent,
                                           u64 parent_badge,
                                           bool is_cross_machine,
                                           struct user_elf *user_elf)
{
        struct proc_node *parent_proc_node;
        struct proc_node *proc_node;
        int caps[3];
        int ret;
        int new_proc_cap;
        int new_proc_mt_cap;
        struct launch_process_args lp_args;

        /* Get parent_proc_node */
        if (if_has_parent) {
                parent_proc_node = get_proc_node(parent_badge);
                assert(parent_proc_node);
        } else {
                parent_proc_node = NULL;
        }

        debug("client: %p, cmd: %s\n", parent_proc_node, input_argv[1]);
        debug("fsm ipc_struct conn_cap=%ld server_type=%d\n",
              fsm_ipc_struct->conn_cap,
              fsm_ipc_struct->server_id);
        info(ANSI_COLOR_MAGENTA "[PROCMGR] Launching %s... (is_cross_machine: %d)" ANSI_COLOR_RESET "\n", name, is_cross_machine);

        /* Init caps */
        caps[0] = __procmgr_server_cap;
        caps[1] = fsm_server_cap;
        caps[2] = lwip_server_cap;

        for (; nr_caps < 3; nr_caps++) {
                if (caps[nr_caps] == 0)
                        break;
        }

        /* Create proc_node to get pid */
        proc_node = new_proc_node(parent_proc_node, name);
        assert(proc_node);

        /* Init launch_process_args */
        lp_args.user_elf = user_elf;
        lp_args.child_process_cap = &new_proc_cap;
        lp_args.child_main_thread_cap = &new_proc_mt_cap;
        lp_args.pmo_map_reqs = NULL;
        lp_args.nr_pmo_map_reqs = 0;
        lp_args.caps = caps;
        lp_args.nr_caps = nr_caps;
        lp_args.cpuid = 0;
        lp_args.argc = input_argc;
        lp_args.argv = input_argv;
        lp_args.badge = proc_node->badge;
        lp_args.pid = proc_node->pid;
        lp_args.pcid = proc_node->pcid;
        lp_args.type = proc_node->thread_type;
        lp_args.is_cross_machine = is_cross_machine;

        /* Launch process */
        // debug("free mem before launch process size: 0x%lx\n",
        ret = launch_process_with_pmos_caps(&lp_args);

        if (ret != 0) {
                error("launch process failed\n");
                return NULL;
        }

        /* Set proc_node properties */
        proc_node->state = PROC_STATE_RUNNING;
        proc_node->proc_cap = new_proc_cap;
        proc_node->proc_mt_cap = new_proc_mt_cap;

        debug("new_proc_node: pid=%d cmd=%s parent_pid=%d\n",
              proc_node->pid,
              proc_node->name,
              proc_node->parent ? proc_node->parent->pid : -1);

        return proc_node;
}

struct proc_node *procmgr_launch_process(int input_argc, char **input_argv,
                                         char *name, bool if_has_parent,
                                         u64 parent_badge, bool is_cross_machine)
{
        int ret;
        struct user_elf user_elf;
        struct proc_node *node;
        int argc = input_argc;
        char *argv[PROC_REQ_ARGC_MAX];
        int argv_start = 1;

        /* Read elf */
        // debug("free mem before readelf size: 0x%lx\n",
        // usys_get_free_mem_size());
        /* argv[0] is fixed as dyn loader. */
        argv[0] = CHCORE_LOADER;

        /* Translate input_argv to argv */
        for (int i = 0; i < argc; ++i)
                argv[argv_start + i] = input_argv[i];

        pthread_mutex_lock(&read_elf_lock);
        ret = readelf_from_fs(input_argv[0], &user_elf, is_cross_machine);
        if (ret < 0 && strcmp(input_argv[0], "/tmpfs.srv") == 0) {
                /* tmpfs.srv is embedded in procmgr, not stored in the FS */
                ret = readelf_from_vaddr(&user_elf,
                                         (size_t)*(u64 *)&__binary_tmpfs_elf_size,
                                         &__binary_tmpfs_elf_start,
                                         is_cross_machine);
                if (ret >= 0)
                        memcpy(user_elf.path, "/tmpfs.srv", sizeof("/tmpfs.srv"));
        }
        pthread_mutex_unlock(&read_elf_lock);

        if (ret < 0) {
                error("No such binary: %s\n", input_argv[0]);
                return NULL;
        }
        debug("got elf from fs ret=%d\n", ret);

        /*
         * If the binary is dynamic, procmgr runs 'libc.so binary'.
         * If the binary is dynamic while it is libc.so or ldd, procmgr runs
         * 'binary'.
         */
        extern int is_dyn_loader(const char *bin_name);
        if (ret == ET_DYN && !is_dyn_loader(argv[argv_start])) {
                ++argc;
                argv_start = 0;
        }

        node = do_launch_process(argc,
                                 &argv[argv_start],
                                 name,
                                 if_has_parent,
                                 parent_badge,
                                 is_cross_machine,
                                 &user_elf);
        return node;
}

struct proc_node *procmgr_launch_basic_server(int input_argc, char **input_argv,
                                              char *name, bool if_has_parent,
                                              u64 parent_badge)
{
        int ret;
        struct proc_node *node;
        struct user_elf user_elf;
        int is_cross_machine = false;

        if (strcmp(input_argv[0], "/tmpfs.srv") == 0) {
                pthread_mutex_lock(&read_elf_lock);
                ret = readelf_from_vaddr(&user_elf,
                                         (size_t)*(u64 *)&__binary_tmpfs_elf_size,
                                         &__binary_tmpfs_elf_start,
                                         is_cross_machine);
                memcpy(user_elf.path, "/tmpfs.srv", sizeof("/tmpfs.srv"));
                pthread_mutex_unlock(&read_elf_lock);
        } else if (strcmp(input_argv[0], "/fsm.srv") == 0) {
                pthread_mutex_lock(&read_elf_lock);
                ret = readelf_from_vaddr(&user_elf,
                                         (size_t)*(u64 *)&__binary_fsm_elf_size,
                                         &__binary_fsm_elf_start,
                                         is_cross_machine);
                memcpy(user_elf.path, "/fsm.srv", sizeof("/fsm.srv"));
                pthread_mutex_unlock(&read_elf_lock);
        } else {
                ret = -1;
        }
        if (ret < 0) {
                debug("Error args or parsing elf error!\n");
                return NULL;
        }
        node = do_launch_process(input_argc,
                                 input_argv,
                                 name,
                                 if_has_parent,
                                 parent_badge,
                                 is_cross_machine,
                                 &user_elf);
        return node;
}

void handle_get_server_cap(ipc_msg_t *ipc_msg, struct proc_request *pr)
{
	int server_cap;
	int ret = 0;

	/* Check if server_id is valid */
	if (pr->server_id >= CONFIG_SERVER_MAX) {
		ipc_return(ipc_msg, -EINVAL);
	}

	pthread_mutex_lock(&sys_server_locks[pr->server_id]);

	/* Boot a new FS server in each PROCMGR_REQ_GET_SERVER_CAP */
	switch (pr->server_id) {
	case SERVER_FAT32_FS: {
		boot_server("fat32", "/fat32.srv", &server_cap);
		ipc_msg->cap_slot_number = 1;
		ipc_set_msg_cap(ipc_msg, 0, server_cap);
		goto out;
	}
	default:
		break;
	}

	server_cap = sys_servers[pr->server_id];

	/* Server already booted */
	if (server_cap != -1) {
		ipc_msg->cap_slot_number = 1;
		ipc_set_msg_cap(ipc_msg, 0, server_cap);
		goto out;
	}
	/* Server not booted */
	else {
		switch (pr->server_id) {
		case SERVER_TMPFS:
			printf("Tmpfs does not start up now.\n");
			BUG_ON(1);
			goto out;
		case SERVER_SYSTEMV_SHMMGR:
			boot_server("posix_shm",
				    "/posix_shm.srv",
				    sys_servers + SERVER_SYSTEMV_SHMMGR);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_SYSTEMV_SHMMGR]);
			goto out;
		case SERVER_HDMI_DRIVER:
#if defined(CHCORE_PLAT_RASPI3) || defined(CHCORE_PLAT_RASPI4)
			boot_server("hdmi",
				    "/hdmi.srv",
				    sys_servers + SERVER_HDMI_DRIVER);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_HDMI_DRIVER]);
#else /* CHCORE_PLAT_RASPI3 || CHCORE_PLAT_RASPI4 */
			error("HDMI NOT supported!\n");
			ret = -1;
#endif /* CHCORE_PLAT_RASPI3 || CHCORE_PLAT_RASPI4 */
			goto out;
		case SERVER_SD_CARD:
			boot_server(
				"sd", "/sd.srv", sys_servers + SERVER_SD_CARD);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_SD_CARD]);
			goto out;
		case SERVER_FAT32_FS:
			boot_server("fat32",
				    "/fat32.srv",
				    sys_servers + SERVER_FAT32_FS);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_FAT32_FS]);
			goto out;
		case SERVER_EXT4_FS:
			boot_server("ext4",
				    "/ext4.srv",
				    sys_servers + SERVER_EXT4_FS);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_EXT4_FS]);
			goto out;
		case SERVER_USB_DEVMGR:
#ifdef CHCORE_PLAT_RASPI3
			error("usb_devmgr should be booted at initialization time.\n");
#else /* CHCORE_PLAT_RASPI3 */
			error("usb_devmgr is only supported on raspi3\n");
#endif /* CHCORE_PLAT_RASPI3 */
			ret = -1;
			goto out;
		case SERVER_SERIAL:
			boot_server("serial",
				    "/serial.srv",
				    sys_servers + SERVER_SERIAL);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_SERIAL]);
			goto out;
		case SERVER_GPIO:
			boot_server(
				"gpio", "/gpio.srv", sys_servers + SERVER_GPIO);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0, sys_servers[SERVER_GPIO]);
			goto out;
		case SERVER_GUI:
#ifdef CHCORE_SERVER_GUI
			boot_server("gui", "/gui.srv",
				sys_servers + SERVER_GUI);
			ipc_msg->cap_slot_number = 1;
			ipc_set_msg_cap(ipc_msg, 0,
					    sys_servers[SERVER_GUI]);
#else /* CHCORE_SERVER_GUI */
			error("GUI NOT enabled!\n");
			ret = -1;
#endif /* CHCORE_SERVER_GUI */
			goto out;
		default:
			error("unvalid server id: %x\n", pr->server_id);
			goto out;
		}
	}

out:
	pthread_mutex_unlock(&sys_server_locks[pr->server_id]);
	if (ret == 0) {
		ipc_return_with_cap(ipc_msg, 0);
	} else {
		ipc_return(ipc_msg, ret);
	}
}

void boot_secondary_servers(void)
{
#ifdef CHCORE_PLAT_RASPI3
#ifdef CHCORE_DRIVER_USB
        boot_server("usb_devmgr", "/uspi.srv", sys_servers + SERVER_USB_DEVMGR);
#endif
#endif /* CHCORE_PLAT_RASPI3 */
}

void init_srvmgr(void)
{
        /* Init read_elf_lock */
        pthread_mutex_init(&read_elf_lock, NULL);
        /* Init server array */
        for (int i = 0; i < CONFIG_SERVER_MAX; ++i) {
                sys_servers[i] = -1;
                pthread_mutex_init(&sys_server_locks[i], NULL);
        }
}
