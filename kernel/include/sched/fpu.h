#pragma once

#include <object/thread.h>
#include <sched/sched.h>

#define EAGER_FPU_MODE 0
#define LAZY_FPU_MODE  1

/* RISC-V only supports eager mode. */
#ifdef CHCORE_ARCH_RISCV64
#define FPU_SAVING_MODE EAGER_FPU_MODE
#else
#define FPU_SAVING_MODE LAZY_FPU_MODE
#endif

void arch_init_thread_fpu(struct thread_ctx *ctx);
void *alloc_fpu_state();

void save_fpu_state(struct thread *thread);
void restore_fpu_state(struct thread *thread);

/* Used when checkpointing a thread */
void copy_fpu_state(void *dst_fpu_state, void *src_fpu_state);

#if FPU_SAVING_MODE == LAZY_FPU_MODE
void disable_fpu_usage(void);
void enable_fpu_usage(void);

/* Used when hanlding FPU traps */
void change_fpu_owner(struct thread *target);
/* Used when migrating a thread from local CPU to other CPUs */
void save_and_release_fpu(struct thread *thread);
#endif
