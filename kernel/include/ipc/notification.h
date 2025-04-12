/* This file defines ds and interfaces related with asynchronous notification */
#pragma once
#include <common/lock.h>
#include <object/thread.h>
#include <common/types.h>
#include <posix/time.h>

struct notification {
    u32 not_delivered_notifc_count;
    u32 waiting_threads_count;
    struct list_head waiting_threads;
    /*
     * notifc_lock protects counter and list of waiting threads,
     * including the internal states of waiting threads.
     */
    struct lock notifc_lock;

    /* For recycling */
#define NOTIFIC_INVALID 0
#define NOTIFIC_VALID   1
    int state;
};

struct irq_notification;

void init_notific(struct notification *notifc);
void deinit_notific(struct notification *notifc);
int wait_notific_internal(struct notification *notifc, bool is_block,
                          struct timespec *timeout, bool need_unlock,
                          bool need_obj_put);
s32 wait_notific(struct notification *notifc, bool is_block,
                 struct timespec *timeout);
s32 signal_notific(struct notification *notifc);
int requeue_notific(struct notification *src_notifc, struct notification *dst_notifc);

void wait_irq_notific(struct irq_notification *notifc);
void signal_irq_notific(struct irq_notification *notifc);

/* Syscalls */
int sys_create_notifc(void);
int wait_notific_internal(struct notification *notifc, bool is_block,
                          struct timespec *timeout, bool need_unlock,
                          bool need_obj_put);
int sys_wait(u32 notifc_cap, bool is_block, struct timespec *timeout);
int sys_notify(u32 notifc_cap);
