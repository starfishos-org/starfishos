#include <sched/fpu.h>
#include <arch/machine/smp.h>
#include <mm/kmalloc.h>

#include "../machine/xsave.h"

/*
 * TODO: on x86_64, STATE_AREA_SIZE should be retrived at boot time
 */
// #define STATE_AREA_SIZE (0x1000)
//char fpu_initial_state[STATE_AREA_SIZE] __attribute__((aligned(64)));

#define USE_FXSAVE   0
#define USE_XSAVEOPT 1

/*
 * TODO: on x86_64, we should use cpuid to detect the supported
 * instructions.
 */
#define USE_INSTRUCTION USE_XSAVEOPT
//#define USE_INSTRUCTION USE_FXSAVE

struct xsave_area {
    /* legacy region */
    u8 legacy_region_0[24];
    u32 mxcsr;
    u8 legacy_region_1[484];

    /* xsave_header */
    u64 xstate_bv;
    u64 xcomp_bv;
    u8 reserved[48];

    u8 extended_region[];
};

#define STATE_AREA_SIZE (sizeof(struct xsave_area))

void *alloc_fpu_state()
{
	return kzalloc(STATE_AREA_SIZE, __DEFAULT__);
}

#if USE_INSTRUCTION == USE_XSAVEOPT
void arch_init_thread_fpu(struct thread_ctx *ctx)
{
	struct xsave_area *area;

	/* should be aligned to 64 */
	ctx->fpu_state = (void *)kzalloc(STATE_AREA_SIZE, __DEFAULT__);
	ctx->is_fpu_owner = -1;

	area = (struct xsave_area *)ctx->fpu_state;
	area->xstate_bv = X86_XSAVE_STATE_SSE;

	/*
	 * the mxcsr register contains control and status information
	 * for the sse registers.
	 *
	 * todo: 0x3f << 7 (0x1f80) is from zircon and sel4.
	 */
	area->mxcsr = 0x3f << 7;
}
#else
void arch_init_thread_fpu(struct thread_ctx *ctx)
{
	/* should be aligned to 64 */
	ctx->fpu_state = (void *)kzalloc(STATE_AREA_SIZE, __DEFAULT__);
	ctx->is_fpu_owner = -1;
}
#endif

void arch_free_thread_fpu(struct thread_ctx *ctx)
{
	kfree(ctx->fpu_state);
}

#if USE_INSTRUCTION == USE_XSAVEOPT
static void xsaveopt(void* register_state, u64 feature_mask)
{
	__asm__ volatile("xsaveopt %0"
			 : "+m"(*(u8 *)register_state)
			 : "d"((u32)(feature_mask >> 32)),
			 "a"((u32)feature_mask)
			 : "memory");

}

static void xrstor(void* register_state, u64 feature_mask)
{
	__asm__ volatile("xrstor %0"
			 :
			 : "m"(*(u8*)register_state),
			 "d"((u32)(feature_mask >> 32)),
			 "a"((u32)feature_mask)
			 : "memory");
}
#endif

static void fxsave(void* register_state)
{
#if USE_INSTRUCTION == USE_XSAVEOPT
	xsaveopt(register_state, ~0UL);
#elif USE_INSTRUCTION == USE_FXSAVE
	__asm__ __volatile__("fxsave %0"
			     : "=m"(*(u8*)register_state)
			     :
			     : "memory");
#else
	BUG("no support FPU\n");
#endif
}

static void fxrstor(void* register_state)
{

#if USE_INSTRUCTION == USE_XSAVEOPT
	xrstor(register_state, ~0UL);
#elif USE_INSTRUCTION == USE_FXSAVE
	__asm__ __volatile__("fxrstor %0"
			     :
			     : "m"(*(u8*)register_state)
			     : "memory");
#else
	BUG("no support FPU\n");
#endif
}

void save_fpu_state(struct thread *thread)
{
	if (likely ((thread)
		    && (thread->thread_ctx->type > TYPE_KERNEL))) {
		thread->thread_ctx->is_fpu_state_modified = 1;
		fxsave(thread->thread_ctx->fpu_state);
	}

}

void restore_fpu_state(struct thread *thread)
{
	if (likely ((thread)
		    && (thread->thread_ctx->type > TYPE_KERNEL))) {

		fxrstor(thread->thread_ctx->fpu_state);
	}
}

#if FPU_SAVING_MODE == LAZY_FPU_MODE

extern struct lock fpu_owner_locks[];
void change_fpu_owner()
{
	struct thread *fpu_owner;
	u32 cpuid;

	cpuid = smp_get_cpu_id();

	/*
	 * Enable FPU for use and it will be disabled again
	 * during thread switching.
	 */
	enable_fpu_usage();

	/*
	 * Use lock to protect the per_cpu fpu_owner which may be
	 * concurrently accessed when freeing a thread that is
	 * fpu_owner of some CPU.
	 */
	lock(&fpu_owner_locks[cpuid]);

	/* Get the fpu_owner of local CPU */
	fpu_owner = (struct thread *)(cpu_info[cpuid].fpu_owner);

	/* A (fpu_owner) -> B (no using fpu) -> A */
	if (fpu_owner == current_thread) {
		unlock(&fpu_owner_locks[cpuid]);
		return;
	}

	/* Save the fpu states for the current (previous) owner */
	if (fpu_owner) {
		BUG_ON(fpu_owner->thread_ctx->type <= TYPE_KERNEL);
		save_fpu_state(fpu_owner);
		/*
		 * The thread fpu_owner will not be modified by other CPU
		 * since it is in the ready queue of current CPU.
		 */
		fpu_owner->thread_ctx->is_fpu_owner = -1;
	}

	fpu_owner = current_thread;
	BUG_ON(fpu_owner->thread_ctx->type <= TYPE_KERNEL);

	/* Set current_thread as the fpu_owner of local CPU */
	cpu_info[cpuid].fpu_owner = (void *)fpu_owner;

	unlock(&fpu_owner_locks[cpuid]);

	restore_fpu_state(current_thread);
	/* Current_thread will not be modified by other CPUs */
	current_thread->thread_ctx->is_fpu_owner = cpuid;
}

/* This interface is specialized for the scheduler to use */
void save_and_release_fpu(struct thread *thread)
{
	struct thread *fpu_owner = NULL;
	u32 cpuid;

	cpuid = smp_get_cpu_id();

	BUG_ON(thread->thread_ctx->thread_exit_state == TE_EXITED);

	/* Get the fpu_owner of local CPU */
	fpu_owner = (struct thread *)(cpu_info[cpuid].fpu_owner);

	/*
	 * @thread will be migrated to other CPUs, and it is the fpu_owner of local CPU.
	 * So, here to save its fpu states.
	 *
	 * No need to acquire the fpu_owner_lock because if fpu_owner == thread,
	 * then the thread will not be freed.
	 */
	if (fpu_owner == thread) {
		/* Enable FPU before operating FPU, otherwise T_NM will be triggered */
		enable_fpu_usage();
		save_fpu_state(fpu_owner);
		fpu_owner->thread_ctx->is_fpu_owner = -1;

		/* Clear the fpu_owner on local CPU */
		fpu_owner = NULL;
		cpu_info[cpuid].fpu_owner = (void *)fpu_owner;
		disable_fpu_usage();
	}
}

#endif

/* use this function to copy fpu state for checkpoint */
void copy_fpu_state(void* dst_fpu_state, void* src_fpu_state)
{
	extern void memcpy_nt(void *dst, void *src, size_t len);
	memcpy_nt(dst_fpu_state, src_fpu_state, STATE_AREA_SIZE);
}
