#pragma once

#include <chcore/type.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pmo_request {
	/* input: args */
	u64 size;
	u64 type;

	/* output: return value */
	u64 ret_cap;
};

struct pmo_map_request {
	/* input: args */
	u64 pmo_cap;
	u64 addr;
	u64 perm;
	/*
	 * If you want to free the pmo cap in current cap goup
	 * after the pmo was mapped to the vmsapce of another
	 * process, please set free_cap to 1.
	 */
	u64 free_cap;

	/* output: return value */
	u64 ret;
};

/* FIXME(MK): This structure is duplicated in kernel/object/thread.c. */
struct thread_args {
	u64 cap_group_cap;
	u64 stack;
	u64 pc;
	u64 arg;
	u32 prio;
	u64 tls;
	u32 type;
};

struct launch_process_args {
	struct user_elf *user_elf;
	int *child_process_cap;
	int *child_main_thread_cap;
	struct pmo_map_request *pmo_map_reqs;
	int nr_pmo_map_reqs;
	int *caps;
	int nr_caps;
	s32 cpuid;
	int argc;
	char **argv;
	u64 badge;
	int pid;
	u64 pcid;
};

/* Fixed badge for root process and servers */
#define ROOT_CAP_GROUP_BADGE (1) /* INIT */
#define PROCMGR_BADGE        ROOT_CAP_GROUP_BADGE
#define FSM_BADGE            (2)
#define LWIP_BADGE           (3)
#define TMPFS_BADGE          (4)

/**
 * Fixed pcid for root process and servers,
 * these should be the same to the definition in cap_group.c
 */
#define ROOT_PROCESS_PCID (1)
#define PROCMGR_PCID      (ROOT_PROCESS_PCID)
#define FSM_PCID          (2)
#define LWIP_PCID         (3)
#define TMPFS_PCID        (4)

int launch_process(struct user_elf *user_elf, int *child_process_cap,
		   int *child_main_thread_cap, u64 badge);

int launch_process_with_pmos_caps(struct launch_process_args *lp_args);

int launch_process_path(const char *path, int *new_thread_cap,
			struct pmo_map_request *pmo_map_reqs,
			int nr_pmo_map_reqs, int caps[], int nr_caps,
			u64 badge);

pid_t chcore_waitpid(pid_t pid, int *status, int options, int d);

#ifdef __cplusplus
}
#endif
