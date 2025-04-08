#pragma once

#include <sched/sched.h>

struct thread_ctx *create_thread_ctx(u32 type, mem_t mem_type);
void destroy_thread_ctx(struct thread *thread);
void init_thread_ctx(struct thread *thread, u64 stack, u64 func, u32 prio,
                     u32 type, s32 aff);

void arch_set_thread_stack(struct thread *thread, u64 stack);
void arch_set_thread_return(struct thread *thread, u64 ret);
u64 arch_get_thread_stack(struct thread *thread);
void arch_set_thread_next_ip(struct thread *thread, u64 ip);
u64 arch_get_thread_next_ip(struct thread *thread);
void arch_set_thread_arg0(struct thread *thread, unsigned long arg);
void arch_set_thread_arg1(struct thread *thread, unsigned long arg);
void arch_set_thread_arg2(struct thread *thread, unsigned long arg);
void arch_set_thread_arg3(struct thread *thread, unsigned long arg);
void arch_set_thread_tls(struct thread *thread, unsigned long tls);
void set_thread_arch_spec_state(struct thread *thread);
void set_thread_arch_spec_state_ipc(struct thread *thread);
void switch_tls_info(struct thread *from, struct thread *to);
