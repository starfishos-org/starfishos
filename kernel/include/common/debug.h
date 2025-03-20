#pragma once

#define OFF 0
#define ON  1

/*
 * When HOOKING_SYSCALL is ON,
 * ChCore will print the syscall number before invoking the handler.
 * Please refer to kernel/syscall/syscall.c.
 */
#define HOOKING_SYSCALL OFF

/*
 * When DETECTING_DOUBLE_FREE_IN_SLAB is ON,
 * ChCore will check each free operation in the slab allocator.
 * Please refer to kernel/mm/slab.c.
 */
#define DETECTING_DOUBLE_FREE_IN_SLAB ON

/*
 * When CHECK_FREE_COUNT_IN_SLAB is ON,
 * ChCore will check each slot's free cnt in the slab allocator.
 * Please refer to kernel/mm/slab.c.
 */
#define CHECK_FREE_COUNT_IN_SLAB ON

/*
 * When ENABLE_BACKTRACE_FUNC is ON,
 * You can use the backtrace function to trace the invoking pipe line.
 * Please refer to kernel/arch/aarch64/backtrace/backtrace.c.
 */
#define ENABLE_BACKTRACE_FUNC OFF

/*
 * When TRACK_THREAD_MM is ON,
 * ChCore will count the physical memory usage of a thread.
 * Note that this is only a debug tool without guaranteeing any semantic.
 */
#define TRACK_THREAD_MM OFF
