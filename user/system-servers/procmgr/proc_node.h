#pragma once

#include <pthread.h>
#include <chcore/container/list.h>
#include <chcore/ipc.h>
#include <chcore-internal/procmgr_defs.h>

#define INIT_BADGE 1

extern pthread_mutex_t recycle_lock;

enum proc_state {
	PROC_STATE_INIT = 1,
	PROC_STATE_RUNNING,
	PROC_STATE_EXIT,
	PROC_STATE_MAX
};

struct proc_node {
	char *name; /* The name of the process. */
	/* The capability of the process owned in procmgr. */
	int proc_cap;
	/* The capability of the process's main thread (MT) owned in procmgr. */
	int proc_mt_cap;
	pid_t pid;
	u64 pcid; /* Used for initializing pgtbl */
	u64 badge; /* Used for recognizing a process. */
	enum proc_state state;
	int exitstatus;

	/* Connecters */
	struct proc_node *parent;
	/* A lock: used to coordinate the access to child procs list */
	pthread_mutex_t lock;
	struct list_head children; /* A list of child procs. */
	struct list_head node; /* The node in the parent's child list. */

	struct hlist_node hash_node; /* node in badge2proc hash table */
};

void init_proc_node_mgr();
struct proc_node *new_proc_node(struct proc_node *parent, char *name);
void del_proc_node(struct proc_node *proc);
void free_proc_node_resource(struct proc_node *proc);
struct proc_node *get_proc_node(u64 client_badge);
void init_root_proc_node();
