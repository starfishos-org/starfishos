#pragma once

#include <arch/sched/arch_sched.h>
#include <arch/sync.h>
#include <common/types.h>
#include <common/list.h>
#include <common/kprint.h>

// TODO: detect CPU num on boot time
#include <machine.h>

struct thread;

/* Timer ticks in system */
#if LOG_LEVEL == DEBUG
/* BUDGET represents the number of TICKs */
#define DEFAULT_BUDGET 1
#define TICK_MS        3000
#else
/* BUDGET represents the number of TICKs */
#define DEFAULT_BUDGET	1
#define TICK_MS		1000
#endif

#define MAX_PRIO     255
#define MIN_PRIO     0
#define PRIO_NUM     (MAX_PRIO + 1)
#define IDLE_PRIO    0
#define DEFAULT_PRIO 1

#define NO_AFF -1

/* Data structures */

#define STATE_STR_LEN 20
enum thread_state {
    TS_INIT = 0,
    TS_READY,
    TS_INTER, /* Intermediate stat used by sched (only for debug) */
    TS_RUNNING,
    TS_EXIT, /* Only for debug use */
    TS_WAITING, /* Waiting IPC or etc */
#ifdef DSM_ENABLED
    TS_MIGRATING, /* migrating to remote */
    TS_STOPPING, /* being stopped by cfork */
    TS_STOPPED, /* has been stopped */
#endif
};

enum kernel_stack_state { KS_FREE = 0, KS_LOCKED };

enum thread_exit_state { TE_RUNNING = 0, TE_EXITING, TE_EXITED };

#define TYPE_STR_LEN 20
enum thread_type {
    /*
     * Kernel-level threads
     * 1. Without FPU states
     * 2. Won't swap gs/fs
     */
    TYPE_IDLE = 0, /* IDLE thread dose not have stack, pause cpu */
    TYPE_KERNEL = 1, /* KERNEL thread has stack */

    /*
     * User-level threads
     * Should be larger than TYPE_KERNEL!
     */
    TYPE_USER = 2,
    TYPE_SHADOW = 3, /* SHADOW thread is used to achieve migrate IPC */
    /* Use as the IPC register callback threads (for recycling) */
    TYPE_REGISTER = 4,
    TYPE_TESTS = 5 /* TESTS thread is used by kernel tests */
};

typedef struct sched_cont {
    u32 budget;
    char pad[pad_to_cache_line(sizeof(u32))];
} sched_cont_t;

/* Must be 8-byte aligned */
struct thread_ctx {
    /* Executing Context */
    arch_exec_cont_t ec;
    /* Is FPU state modified ?
     * set 0 during checkpoint;
     * set 1 if call save_fpu_state;
     */
    u32 is_fpu_state_modified;
    /* FPU States */
    void *fpu_state;
    /* Is FPU owner on some CPU: -1 means No; other means CPU ID */
    int is_fpu_owner;
    /* TLS Related States */
    u64 tls_base_reg[TLS_REG_NUM];
    /* Scheduling Context */
    sched_cont_t *sc;
    /* Thread Type */
    u32 type;
    /* Thread state (can not be modified by other cores) */
    u32 state;
    /* Priority */
    u32 prio;
    /* SMP Affinity */
    s32 affinity;
    /* Current Assigned CPU */
    u32 cpuid;
    /* Thread kernel stack state */
    volatile u32 kernel_stack_state;
    /* Thread exit state */
    volatile u32 thread_exit_state;
} __attribute__((aligned(CACHELINE_SZ)));

/* Debug functions */
void print_thread(struct thread *thread);

extern char thread_type[][TYPE_STR_LEN];
extern char thread_state[][STATE_STR_LEN];

void arch_idle_ctx_init(struct thread_ctx *idle_ctx, u64 stack,
                        void (*func)(void));
void arch_switch_context(struct thread *target);
u64 switch_context(void);

/* This interface is local to scheduler. */
int switch_to_thread(struct thread *target);

/* This interface can be used in other places in the kernel. */
void sched_to_thread(struct thread *target);

/* Helper functions */
s32 get_cpubind(struct thread *thread);
struct thread *find_runnable_thread(struct list_head *thread_list);
void add_pending_resched(u32 cpuid);
void wait_for_kernel_stack(struct thread *thread);

/* Global-shared kernel data */
extern struct list_head ready_queue[PLAT_CPU_NUM][PRIO_NUM];
extern struct thread *current_threads[PLAT_CPU_NUM];

/* Indirect function call may downgrade performance */
struct sched_ops {
    int (*sched_init)(void);
    int (*sched)(void);
    int (*sched_enqueue)(struct thread *thread);
    int (*sched_dequeue)(struct thread *thread);
    /* Debug tools */
    void (*sched_top)(void);
    /* CKPT restore tools */
    void (*sched_clear)(void);
};

/* Provided Scheduling Policies */
extern struct sched_ops pbrr; /* Priority Based Round Robin */
extern struct sched_ops rr; /* Simple Round Robin */

/* Chosen Scheduling Policies */
extern struct sched_ops *cur_sched_ops;

int sched_init(struct sched_ops *sched_ops);

static inline int sched(void)
{
    return cur_sched_ops->sched();
}

static inline int sched_enqueue(struct thread *thread)
{
    return cur_sched_ops->sched_enqueue(thread);
}

static inline int sched_dequeue(struct thread *thread)
{
    return cur_sched_ops->sched_dequeue(thread);
}

static inline void sched_clear(void)
{
    cur_sched_ops->sched_clear();
}

/* Syscalls */
void sys_yield(void);
void sys_top(void);
void sys_perf_null(void);

int rr_sched_migrate_to_remote(struct thread *thread);

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
