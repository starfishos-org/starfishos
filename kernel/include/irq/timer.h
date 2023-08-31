#pragma once

#include <common/types.h>
#include <common/list.h>
#include <common/lock.h>
#include <machine.h>
#include <posix/time.h>

struct thread;

typedef void (*timer_cb)(struct thread *thread);

/* Every thread has a sleep_state struct */
struct sleep_state {
	/* Link sleeping threads on each core */
	struct list_head sleep_node;
	/* Time to wake up */
	u64 wakeup_tick;
	/* The cpu id where the thread is sleeping */
	u64 sleep_cpu;
	/*
	 * When the timeout is caused during notification wait (with timeout),
	 * The following field indicates which notification it is waiting for.
	 * Currently, we record this field for removing the timeout thread from
	 * the waiting queue of the corresponding notification.
	 */
	struct notification *pending_notific;
	/*
	 * Currently 2 type of callbacks: notification and sleep.
	 * If it is NULL, the thread is not waiting for timeout.
	 */
	timer_cb cb;

	/*
	 * Get this lock before inserting or removing the thread in or from
	 * a waiting list, i.e., sleep_list and wait_list of a notification.
	 */
	struct lock queue_lock;
};

#define NS_IN_S		(1000000000UL)
#define US_IN_S		(1000000UL)
#define NS_IN_US	(1000UL)

extern struct list_head sleep_lists[PLAT_CPU_NUM];
extern u64 tick_per_us;
void sleep_state_enqueue(struct sleep_state *sleep_state); // TODO(FN): rename?
int enqueue_sleeper(struct thread *thread, const struct timespec *timeout,
		    timer_cb cb);
bool try_dequeue_sleeper(struct thread *thread);

void timer_init(void);
void plat_timer_init(void);
void plat_set_next_timer(u64 tick_delta);
void handle_timer_irq(void);
void plat_handle_timer_irq(u64 tick_delta);

u64 plat_get_mono_time(void);
u64 plat_get_current_tick(void);

/* Syscalls */
int sys_clock_gettime(clockid_t clock, struct timespec *ts);
int sys_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req,
                        struct timespec *rem);
