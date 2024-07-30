#include <chcore/syscall.h>
#include <stdio.h>
#include <chcore/defs.h>
#include <chcore/type.h>
#include <chcore/proc.h>

/*
 * TODO: use vmalloc like function (safely use virtual space).
 * TODO: add guard page to different areas (e.g,, stack area).
 */

int thread_num_in_cap_group = 0;

/*
 * TODO: remove this deprecated interface later.
 *
 * when mix using pthread_create, it will increase a varible in
 * musl-libc (i.e., libc.threads_minus_1).
 * We should follow that convention, otherwise no concurrency control.
 */
int create_thread(void *(*func)(void *), u64 arg, u32 prio, u64 tls)
{
	int child_stack_pmo_cap = 0;
	int child_thread_cap = 0;
	int ret = 0;

	/* Thread ID */
	int tid = 0;
	u64 thread_stack_base = 0;
	u64 thread_stack_top = 0;

	struct thread_args args;

	printf("[warning] %s is deprecated: it does not compatabile with libc and"
	       " it does not initialize per-thread ipc connection.\n", __func__);

	tid = __sync_fetch_and_add(&thread_num_in_cap_group, 1);

	child_stack_pmo_cap = usys_create_pmo(CHILD_THREAD_STACK_SIZE,
					      PMO_ANONYM, MALLOC_TYPE_DEFAULT);
	if (child_stack_pmo_cap < 0)
		return child_stack_pmo_cap;

	thread_stack_base = CHILD_THREAD_STACK_BASE
		            + tid * CHILD_THREAD_STACK_SIZE;

	ret = usys_map_pmo(SELF_CAP,
			   child_stack_pmo_cap,
			   thread_stack_base,
			   VM_READ | VM_WRITE);
	if (ret < 0)
		return ret;

	/*
	 * GCC assumes the stack frame are aligned to 16-byte.
	 * When invoking a function with call instruction,
	 * the stack should align to 8-byte (not 16).
	 * Otherwise, alignment error may occur
	 * (at least on x86_64).
	 * Besides, ChCore disables alignment check on aarch64.
	 *
	 * Thus, we deliberately set the initial SP here.
	 */
	thread_stack_top = thread_stack_base + CHILD_THREAD_STACK_SIZE;
	thread_stack_top -= 8;

	args.cap_group_cap = SELF_CAP;
	args.stack = thread_stack_top;
	args.pc = (u64)func;
	args.arg = arg;
	args.prio = prio;
	args.tls = 0;
	args.type = 0;
	child_thread_cap = usys_create_thread((u64)&args);

	return child_thread_cap;
}
