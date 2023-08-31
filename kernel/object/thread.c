#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/util.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/uaccess.h>
#include <mm/vmspace.h>
#include <object/thread.h>
#include <object/recycle.h>
#include <sched/context.h>
#include <arch/machine/registers.h>
#include <arch/machine/smp.h>
#include <arch/time.h>
#include <irq/ipi.h>
#include <common/endianness.h>
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>

#include "thread_env.h"

extern const char binary_procmgr_bin_start;
extern const char binary_procmgr_bin_size;

/*
 * local functions
 */
#ifdef CHCORE
static int thread_init(struct thread *thread, struct cap_group *cap_group,
#else /* For unit test */
int thread_init(struct thread *thread, struct cap_group *cap_group,
#endif
		u64 stack, u64 pc, u32 prio, u32 type, s32 aff)
{
	if (prio >= PRIO_NUM) {
		return -EINVAL;
	}
	if (aff != NO_AFF && (aff < 0 || aff >= PLAT_CPU_NUM)) {
		return -EINVAL;
	}
	
	thread->cap_group = obj_get(cap_group, CAP_GROUP_OBJ_ID,
				    TYPE_CAP_GROUP);
	thread->vmspace = obj_get(cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
	obj_put(thread->cap_group);
	obj_put(thread->vmspace);

#ifdef OMIT_BENCHMARK
	// printk("cap_group_name=%s\n", thread->cap_group->cap_group_name);
	if (!strcmp(thread->cap_group->cap_group_name, "/redis-benchmark")) {
		redis_benchmark_vmspace = thread->vmspace;
		// printk("redis_benchmark_vmspace=%p\n", redis_benchmark_vmspace);
	}
	if (!strcmp(thread->cap_group->cap_group_name, "/memcachetest")) {
		memcachetest_vmspace = thread->vmspace;
		// printk("memcachetest_vmspace=%p\n", memcachetest_vmspace);
	}
	if (!strcmp(thread->cap_group->cap_group_name, "/ycsbc")) {
		ycsb_vmspace = thread->vmspace;
	}
#endif
	/* Thread context is used as the kernel stack for that thread */
	thread->thread_ctx = create_thread_ctx(type);
	if (!thread->thread_ctx)
		return -ENOMEM;
	init_thread_ctx(thread, stack, pc, prio, type, aff);

	/*
	 * Field prev_thread records the previous thread runs
	 * just before this thread. Obviously, it is NULL at the beginning.
	 */
	thread->prev_thread = NULL;

	/* The ipc_config will be allocated on demand */
	thread->general_ipc_config = NULL;

	thread->sleep_state.cb = NULL;

#ifdef TRACK_TIME
	thread->tracking = 0;
	thread->track_time_kernel = 0;
	thread->track_time_user = 0;
#endif

	lock_init(&thread->sleep_state.queue_lock);

	return 0;
}

void thread_deinit(void *thread_ptr)
{
	struct thread *thread;
	struct cap_group *cap_group;

	thread = (struct thread *)thread_ptr;

	BUG_ON(thread->thread_ctx->thread_exit_state != TE_EXITED);
	if (thread->thread_ctx->state != TS_EXIT)
		kwarn("thread ctx->state is %d\n", thread->thread_ctx->state);

	cap_group = thread->cap_group;

#ifdef CKPT_CAP_GROUP_LAZY_COPY
	cap_group_lazy_copy_ckpt(cap_group);
#endif

	lock(&cap_group->threads_lock);
	list_del(&thread->node);
	unlock(&cap_group->threads_lock);

	if (thread->general_ipc_config)
		kfree(thread->general_ipc_config);

	destroy_thread_ctx(thread);

	/* The thread struct itself will be freed in __free_object */
}

#define PFLAGS2VMRFLAGS(PF)                                                    \
	(((PF)&PF_X ? VMR_EXEC : 0) | ((PF)&PF_W ? VMR_WRITE : 0) |            \
	 ((PF)&PF_R ? VMR_READ : 0))

#define OFFSET_MASK (0xFFF)

/* Defined in page_table.S (maybe required on aarch64) */
extern void flush_idcache(void);

/* Required by LibC */
void prepare_env(char *env, u64 top_vaddr, char *name, struct process_metadata *meta);

/*
 * exported functions
 */
void switch_thread_vmspace_to(struct thread *thread)
{
	switch_vmspace_to(thread->vmspace);
}

/* Arguments for the inital thread */
#define ROOT_THREAD_STACK_BASE		(0x500000000000UL)
#define ROOT_THREAD_STACK_SIZE		(0x800000UL)
#define ROOT_THREAD_PRIO		DEFAULT_PRIO

#define ROOT_THREAD_VADDR		0x400000

char ROOT_NAME[] = "/procmgr.srv";

/*
 * The root_thread is actually a first user thread
 * which has no difference with other user threads
 */
void create_root_thread(void)
{
	struct cap_group *root_cap_group;
	int thread_cap;
	struct thread *root_thread;
	char data[8];
	u64 file_size;
	u64 mem_size;
	struct pmobject *pmo;
	int ret;
	int stack_pmo_cap;
	struct thread *thread;
	struct pmobject *stack_pmo;
	struct vmspace *init_vmspace;
	u64 stack;
	vaddr_t kva;
	struct process_metadata meta;


	/*
	 * Read from binary.
	 * The msg and the binary of of the init process(procmgr) are linked
	 * behind the kernel image via the incbine instruction.
	 * The binary_procmgr_bin_start points to the first piece of info:
	 * the entry point of the init process, followed by eight bytes of data
	 * that stores the mem_size of the binary.
	 */
	memcpy(data, &binary_procmgr_bin_start, 8);
	mem_size = be64_to_cpu(*(u64 *)data);

	memcpy(data, (void *)((u64)&binary_procmgr_bin_start + 8), 8);
	meta.entry = be64_to_cpu(*(u64 *)data);

	memcpy(data, (void *)((u64)&binary_procmgr_bin_start + 16), 8);
	meta.flags = be64_to_cpu(*(u64 *)data);

	memcpy(data, (void *)((u64)&binary_procmgr_bin_start + 24), 8);
	meta.phentsize = be64_to_cpu(*(u64 *)data);

	memcpy(data, (void *)((u64)&binary_procmgr_bin_start + 32), 8);
	meta.phnum = be64_to_cpu(*(u64 *)data);

	memcpy(data, (void *)((u64)&binary_procmgr_bin_start + 40), 8);
	meta.phdr_addr = be64_to_cpu(*(u64 *)data);

	printk("%lx %lx %lx %lx %lx %lx\n", mem_size, meta.entry, meta.flags, meta.phentsize, meta.phnum, meta.phdr_addr);
	file_size = *(u64 *)&binary_procmgr_bin_size - 48;

	root_cap_group = create_root_cap_group(ROOT_NAME, strlen(ROOT_NAME));

	/* For ckpt */
	root_cap_group_obj_for_ckpt = container_of(root_cap_group, struct object, opaque);
	
	ret = create_pmo(ROUND_UP(mem_size, PAGE_SIZE), PMO_DATA, root_cap_group, &pmo);
	BUG_ON(ret < 0);
	memset((void *)phys_to_virt(pmo->start), 0, pmo->size);
	memcpy((void *)phys_to_virt(pmo->start), (void *)(((u64)&binary_procmgr_bin_start) + 48), file_size);

	init_vmspace = obj_get(root_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
	obj_put(init_vmspace);

	/* Allocate and setup a user stack for the init thread */
	stack_pmo_cap = create_pmo(ROOT_THREAD_STACK_SIZE, PMO_ANONYM,
				root_cap_group, &stack_pmo);
	BUG_ON(stack_pmo_cap < 0);

	ret = vmspace_map_range(init_vmspace, ROOT_THREAD_STACK_BASE, ROOT_THREAD_STACK_SIZE,
				VMR_READ | VMR_WRITE, stack_pmo);
	BUG_ON(ret != 0);

	/* Allocate the init thread */
	thread = obj_alloc(TYPE_THREAD, sizeof(*thread));
	BUG_ON(thread == NULL);

	/* Fill the parameter of the thread struct */
	ret = vmspace_map_range(init_vmspace,
				ROOT_THREAD_VADDR,
				pmo->size,
				VMR_READ | VMR_WRITE | VMR_EXEC,
				pmo);
	BUG_ON(ret < 0);

	stack = ROOT_THREAD_STACK_BASE + ROOT_THREAD_STACK_SIZE;

	/* Allocate a physical for the main stack for prepare_env */
	// kva = (vaddr_t)get_dram_pages(0);
	kva = (vaddr_t)get_pages(0);
	BUG_ON(kva == 0);
	commit_page_to_pmo(stack_pmo, ROOT_THREAD_STACK_SIZE / PAGE_SIZE - 1,
			virt_to_phys((void *)kva));

	prepare_env((char *)kva, stack, ROOT_NAME, &meta);
	stack -= ENV_SIZE_ON_STACK;

	ret = thread_init(thread, root_cap_group, stack, meta.entry, ROOT_THREAD_PRIO, TYPE_USER, smp_get_cpu_id());
	BUG_ON(ret != 0);

	/* Add the thread into the thread_list of the cap_group */
	lock(&root_cap_group->threads_lock);
	list_add(&thread->node, &root_cap_group->thread_list);
	root_cap_group->thread_cnt += 1;
	unlock(&root_cap_group->threads_lock);


	/* Allocate the cap for the init thread */
	thread_cap = cap_alloc(root_cap_group, thread, 0);
	BUG_ON(thread_cap < 0);

	/* L1 icache & dcache have no coherence on aarch64 */
	flush_idcache();

	root_thread = obj_get(root_cap_group, thread_cap, TYPE_THREAD);
	/* Enqueue: put init thread into the ready queue */
	BUG_ON(sched_enqueue(root_thread));
	obj_put(root_thread);
}

/*
 * create a thread in some process
 * return the thread_cap in the target cap_group
 * TODO: run/stop control; whether the cap in current cap_group is required
 */

static int create_thread(struct cap_group *cap_group,
		  u64 stack, u64 pc, u64 arg, u32 prio, u32 type, u64 tls)
{
#ifdef CKPT_CAP_GROUP_LAZY_COPY
	cap_group_lazy_copy_ckpt(cap_group);
#endif
	struct thread *thread;
	int cap, ret = 0;

	if (!cap_group) {
		ret = -ECAPBILITY;
		goto out_fail;
	}
	thread = obj_alloc(TYPE_THREAD, sizeof(*thread));
	if (!thread) {
		ret = -ENOMEM;
		goto out_obj_put;
	}

	/* Set redis-server to CPU2 and redis-benchmark to CPU3 */
	if(tls == 2 || tls == 3) {
		ret = thread_init(thread, cap_group, stack, pc, prio, type, tls);
	} else {
		ret = thread_init(thread, cap_group, stack, pc, prio, type, NO_AFF);
	}	
	if (ret != 0)
		goto out_free_obj;

	lock(&cap_group->threads_lock);

	/*
	 * Check the exiting state: do not create new threads if exiting (e.g.,
	 * after sys_exit_group is executed.
	 */
	if (current_thread->thread_ctx->thread_exit_state == TE_EXITING) {
		unlock(&cap_group->threads_lock);
		obj_free(thread);
		obj_put(cap_group);
		sched();
		eret_to_thread(switch_context());
	}

	list_add(&thread->node, &cap_group->thread_list);
	cap_group->thread_cnt += 1;
	unlock(&cap_group->threads_lock);

	arch_set_thread_arg0(thread, arg);

	/* set thread tls */
	arch_set_thread_tls(thread, tls);

	/* set arch-specific thread state */
	set_thread_arch_spec_state(thread);

	/* cap is thread_cap in the target cap_group */
	cap = cap_alloc(cap_group, thread, 0);
	if (cap < 0) {
		ret = cap;
		goto out_free_obj;
	}
	thread->cap = cap;

	/* ret is thread_cap in the current_cap_group */
	if (cap_group != current_cap_group)
		cap = cap_copy(cap_group, current_cap_group, cap);
	if (type == TYPE_USER) {
		thread->thread_ctx->state = TS_INTER;
		BUG_ON(sched_enqueue(thread));
	} else if ((type == TYPE_SHADOW) || (type == TYPE_REGISTER)) {
		thread->thread_ctx->state = TS_WAITING;
	}
	return cap;

out_free_obj:
	obj_free(thread);
out_obj_put:
	obj_put(cap_group);
out_fail:
	return ret;
}

void thread_clone(struct cap_group *cap_group, struct thread *thread)
{
	int ret = 0;

	if (!cap_group) {
		return;
	}

	thread->cap_group = cap_group;
	thread->vmspace = obj_get(cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
	obj_put(thread->vmspace);

	ret = thread_init(thread, cap_group, 0,0, current_thread->thread_ctx->prio, TYPE_USER, NO_AFF);
	if (ret != 0)
		goto out_free_thread;

	/* Copy the thread's execution context and tls */
	memcpy(&thread->thread_ctx->ec, &current_thread->thread_ctx->ec, ARCH_EXEC_CONT_SIZE);
	memcpy(&thread->thread_ctx->tls_base_reg,  &current_thread->thread_ctx->tls_base_reg,
	       sizeof(thread->thread_ctx->tls_base_reg));

	BUG_ON(current_thread->thread_ctx->thread_exit_state == TE_EXITING);
	list_add(&thread->node, &cap_group->thread_list);
	cap_group->thread_cnt += 1;

	/* The return value for cloned thread should be zero */
	arch_set_thread_return(thread, 0);
	thread->thread_ctx->state = TS_INTER;
	BUG_ON(sched_enqueue(thread));
	return;
out_free_thread:
	obj_free(thread);
}

/**
 * FIXME(MK): This structure is duplicated in chcore musl headers.
 */
struct thread_args {
	u64 cap_group_cap;
	u64 stack;
	u64 pc;
	u64 arg;
	u32 prio;
	u64 tls;
	/* 0: TYPE_USER; 1: TYPE_SHADOW; 2: TYPE_REGISTER */
	u32 type;
};

/*
 * Create a pthread in some process
 * return the thread_cap in the target cap_group
 * TODO: run/stop control; whether the cap in current cap_group is required
 */
int sys_create_thread(u64 thread_args_p)
{
	struct thread_args args = {0};
	struct cap_group *cap_group;
	int thread_cap;
	int r;
	u32 type;

	r = copy_from_user((char *)&args, (char *)thread_args_p, sizeof(args));
	BUG_ON(r);

	cap_group = obj_get(current_cap_group, args.cap_group_cap,
			    TYPE_CAP_GROUP);

	switch (args.type)
	{
	case 0:
		type = TYPE_USER;
		break;
	case 1:
		type = TYPE_SHADOW;
		break;
	case 2:
		type = TYPE_REGISTER;
		break;
	default:
		kinfo("%s: invalid thread type.\n", __func__);
		thread_cap = -EINVAL;
		goto out;
	}

	thread_cap = create_thread(cap_group, args.stack, args.pc,
				   args.arg, args.prio, type,
				   args.tls);

out:
	obj_put(cap_group);
	return thread_cap;
}

/* Exit the current running thread */
void sys_thread_exit(void)
{
#ifdef CKPT_CAP_GROUP_LAZY_COPY
	cap_group_lazy_copy_ckpt(current_cap_group);
#endif
	
	int cnt;

	/* As a normal application, the main thread will eventually invoke
	 * sys_exit_group or trigger unrecoverable fault (e.g., segfault).
	 *
	 * However a malicious application, all of its thread may invoke
	 * sys_thread_exit. So, we monitor the number of non-shadow threads
	 * in a cap_group (as a user process now).
	 */

	kdebug("%s is invoked\n", __func__);

	/* Set thread state, which will be recycle afterwards */
	current_thread->thread_ctx->thread_exit_state = TE_EXITING;

	lock(&(current_cap_group->threads_lock));
	cnt = --current_cap_group->thread_cnt;
	unlock(&(current_cap_group->threads_lock));

#ifdef TRACK_TIME
	printk("<track> thread of %s: kernel %d ms, user %d ms\n", current_cap_group->cap_group_name, current_thread->track_time_kernel, current_thread->track_time_user);
#endif

	if (cnt == 0) {
		/*
		 * Current thread is the last thread in this cap_group,
		 * so we invoke sys_exit_group.
		 */
		kdebug("%s invokes sys_exit_group\n", __func__);
		sys_exit_group(0);
		/* The control flow will not go through */
	}

	kdebug("%s invokes sched\n", __func__);
	/* Reschedule */
	sched();
	eret_to_thread(switch_context());
}

int sys_set_affinity(u64 thread_cap, s32 aff)
{
	struct thread *thread = NULL;
	int ret = 0;

	/* XXX: currently, we use -1 to represent the current thread */
	if (thread_cap == -1) {
		thread = current_thread;
		BUG_ON(!thread);
	} else {
		thread = obj_get(current_cap_group, thread_cap, TYPE_THREAD);
	}

	if (thread == NULL) {
		ret = -ECAPBILITY;
		goto out;
	}

	/* Check aff */
	if (aff >= PLAT_CPU_NUM) {
		ret = -EINVAL;
		goto out_obj_put;
	}
	thread->thread_ctx->affinity = aff;
out_obj_put:
	if (thread_cap != -1)
		obj_put((void *)thread);
out:
	return ret;
}

s32 sys_get_affinity(u64 thread_cap)
{
	struct thread *thread = NULL;
	s32 aff = 0;

	/* XXX: currently, we use -1 to represent the current thread */
	if (thread_cap == -1) {
		thread = current_thread;
		BUG_ON(!thread);
	} else {
		thread = obj_get(current_cap_group, thread_cap, TYPE_THREAD);
	}

	if (thread == NULL)
		return -ECAPBILITY;

	aff = thread->thread_ctx->affinity;

	if (thread_cap != -1)
		obj_put((void *)thread);
	return aff;
}
