#include <assert.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <chcore/idman.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/container/hashtable.h>
#include <chcore/proc.h>
#include <chcore/uapi/thread.h>

#include "proc_node.h"
#include "procmgr_dbg.h"

extern int __procmgr_server_cap;

/* For synchronization */
static pthread_mutex_t proc_nodes_lock;

/* For allocating pid to proc_node */
static struct id_manager pid_mgr;
static const int PID_MAX = 1024 * 1024;

/* Map: client_badge -> proc_node */
/*
 * We use client_badge as the index of procnode. Hash table use u32 key but the
 * client_badge is u64. It is OK because that the lower 32 bits of client_badge
 * must be different.(badge = (rand() << 10) + pid)
 */
static struct htable badge2proc;

/* Max number of pcid on x86_64 is different from that (ASID) on aarch64. */
static struct id_manager pcid_mgr;
#if defined(CHCORE_ARCH_X86_64)
static const int PCID_MAX = 1 << 12;
#elif defined(CHCORE_ARCH_AARCH64)
static const int PCID_MAX = 1 << 16;
#elif defined(CHCORE_ARCH_RISCV64)
static const int PCID_MAX = 1 << 16;
#else
#error "Unsupported architecture"
#endif

#define HASH_TABLE_SIZE 509
/*
 * PCID in range [0,10) is reserved for boot page table, root process,
 * fsm, fs, lwip, procmgr and future servers (pcid 0 is not used).
 * Thus the user apps' pcid starts at 10. (10 is used by init process)
 */
static const int MAX_RESERVED_PCID = 10;

/* Only for handle_init */
static struct proc_node *proc_init;

static inline u64 generate_badge(struct proc_node *proc)
{
	unsigned state = (unsigned)time(NULL);
	return (rand_r(&state) << 10) + proc->pid;
}

static struct proc_node *__new_proc_node(struct proc_node *parent, char *name)
{
	struct proc_node *proc = malloc(sizeof(*proc));
	assert(proc);

	proc->name = name;
	proc->parent = parent;
	pthread_mutex_init(&proc->lock, NULL);
	if (proc->parent) {
		/* Add this proc to parent's child list. */
		pthread_mutex_lock(&parent->lock);
		list_add(&proc->node, &proc->parent->children);
		pthread_mutex_unlock(&parent->lock);
	}
	init_list_head(&proc->children);
	init_hlist_node(&proc->hash_node);

	proc->state = PROC_STATE_INIT;

	return proc;
}

void init_proc_node_mgr()
{
	init_id_manager(&pid_mgr, PID_MAX, DEFAULT_INIT_ID);

	pthread_mutex_init(&proc_nodes_lock, NULL);
	pthread_mutex_init(&recycle_lock, NULL);
	/* Reserve the pcid for root process and servers. */
	init_id_manager(&pcid_mgr, PCID_MAX, MAX_RESERVED_PCID);

	init_htable(&badge2proc, HASH_TABLE_SIZE);
}

/*
 * The name here should be a newly allocated memory that can be directly stored
 * (and sometime later freed) in the proc_node.
 */
struct proc_node *new_proc_node(struct proc_node *parent, char *name)
{
	struct proc_node *proc = __new_proc_node(parent, name);

	pthread_mutex_lock(&proc_nodes_lock);

	/* Alloc pid */
	proc->pid = alloc_id(&pid_mgr);
	BUG_ON(proc->pid == -EINVAL);

	/* Alloc pcid */
	if (strcmp(name, "procmgr") == 0) {
		proc->pcid = PROCMGR_PCID;
		proc->badge = PROCMGR_BADGE;
		proc->thread_type = THREAD_TYPE_SERVICES;
	} else if (strcmp(name, "fsm") == 0) {
		proc->pcid = FSM_PCID;
		proc->badge = FSM_BADGE;
		proc->thread_type = THREAD_TYPE_SERVICES;
	} else if (strcmp(name, "lwip") == 0) {
		proc->pcid = LWIP_PCID;
		proc->badge = LWIP_BADGE;
		proc->thread_type = THREAD_TYPE_SERVICES;
	} else if (strcmp(name, "tmpfs") == 0) {
		proc->pcid = alloc_id(&pcid_mgr);
		proc->badge = generate_badge(proc);
		proc->thread_type = THREAD_TYPE_SERVICES;
	} else {
		proc->pcid = alloc_id(&pcid_mgr);
		proc->badge = generate_badge(proc);
		proc->thread_type = THREAD_TYPE_USER;
	}
	BUG_ON(proc->pcid == -EINVAL);

	/* Generate badge and add to htable */
	htable_add(&badge2proc, proc->badge, &proc->hash_node);

	pthread_mutex_unlock(&proc_nodes_lock);
	debug("alloc pcid = %ld\n", proc->pcid);
	return proc;
}

void free_proc_node_resource(struct proc_node *proc)
{
	int pid;
	int pcid;

	pthread_mutex_lock(&proc_nodes_lock);

	pid = proc->pid;
	pcid = (int)proc->pcid;

	/* Just delete the node in the hash table. Free proc later. */
	htable_del(&proc->hash_node);

	free_id(&pid_mgr, pid);
	free_id(&pcid_mgr, pcid);
	debug("free pcid = %d\n", pcid);

	if (proc->name)
		free(proc->name);
	pthread_mutex_unlock(&proc_nodes_lock);
}

/* Free the resource allocated in new_proc_node in a reverse order */
void del_proc_node(struct proc_node *proc)
{
	struct proc_node *child;
	struct proc_node *tmp;

	BUG_ON(proc->state != PROC_STATE_EXIT);

	/*
	 * Step 1. Set the child proc node as orphan, delete the child list of
	 * the proc node and free all exited child node.
	 */
	for_each_in_list_safe(child, tmp, node, &proc->children) {
		child->parent = NULL;
		/*
		 * NOTE: If we need keep the relationship between the child of the
		 * proc_node, we need append the child node to a new process(such
		 * as init).
		 */
		/* Recycle exited child proc node. */
		if (child->state == PROC_STATE_EXIT) {
			free_proc_node_resource(child);
			free(child);
		}
	}

	/*
	 * Step2. If the proc is orphan, free the proc node.
	 */
	if (!proc->parent) {
		free_proc_node_resource(proc);
		free(proc);
	}
}

struct proc_node *get_proc_node(u64 client_badge)
{
	struct proc_node *proc;
	struct hlist_head *buckets;

	pthread_mutex_lock(&proc_nodes_lock);
	buckets = htable_get_bucket(&badge2proc, client_badge);

	for_each_in_hlist(proc, hash_node, buckets) {
		if (client_badge == proc->badge) {
			goto out;
		}
	}
	/* NOTE: It should be reconstroction. */
	pthread_mutex_unlock(&proc_nodes_lock);
	return NULL;
out:
	debug("Find badge = 0x%lx, get proc = %p\n", client_badge, proc);
	pthread_mutex_unlock(&proc_nodes_lock);

	return proc;
}

void init_root_proc_node()
{
	/* Init the init node. new_proc_node already adds it to badge2proc. */
	proc_init = new_proc_node(NULL, strdup("procmgr"));

	__sync_synchronize();
}
