#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <chcore/syscall.h>
#include <chcore/type.h>

#define DEFAULT_SAMPLES 50
#define DEFAULT_LOCAL_CPU 4
#define DEFAULT_REMOTE_CPU 12
#define SHARED_STATE_SIZE  4096
#define WORKER_STACK_SIZE  (1UL << 20)

#ifndef MAP_FLAG_SHARED
#define MAP_FLAG_SHARED 0x200000
#endif

struct sched_sample {
    volatile u64 start_ns;
    volatile int done;
    volatile int affinity;
    int remote_cpu;
};

struct notify_state {
    volatile int ready;
    volatile int done;
    volatile int error;
    int samples;
    int remote_cpu;
    int notifc_cap;
};

static u64 mono_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static void *alloc_shared_region(size_t size)
{
    void *state = mmap(NULL,
                       size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_SHARED,
                       -1,
                       0);

    if (state == MAP_FAILED || state == NULL)
        return NULL;
    memset(state, 0, size);
    return state;
}

static void free_shared_region(void *state, size_t size)
{
    if (state)
        munmap(state, size);
}

static void wait_until_at_least(volatile int *value, int expected)
{
    unsigned int spins = 0;

    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < expected) {
        /* The completion timestamp is taken by this source-side observer.
         * Poll continuously so a local yield is not charged to the remote
         * scheduling/notification path.  The rare yield is only a watchdog. */
        if ((++spins & 0xfffff) == 0) {
            usys_yield();
        }
    }
}

static void wait_until_at_least_yielding(volatile int *value, int expected)
{
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < expected) {
        usys_yield();
    }
}

static void *sched_worker(void *opaque)
{
    struct sched_sample *sample = opaque;
    u64 start;

    start = mono_ns();
    __atomic_store_n(&sample->start_ns, start, __ATOMIC_RELEASE);

    if (usys_set_affinity(-2, sample->remote_cpu) != 0) {
        __atomic_store_n(&sample->affinity, -1, __ATOMIC_RELEASE);
        __atomic_store_n(&sample->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    /* set_affinity marks the thread as migrating.  The yield is the point at
     * which it leaves the source scheduler; execution below proves that the
     * thread has actually resumed in user mode on the destination machine. */
    usys_yield();
    __atomic_store_n(&sample->affinity,
                     usys_get_affinity(-1),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&sample->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int run_sched_samples(int samples, int target_cpu, const char *metric)
{
    int i;
    const char *label = metric ? metric : "sched_warmup";

    for (i = 0; i < samples; ++i) {
        struct sched_sample *sample;
        pthread_t worker;
        pthread_attr_t attr;
        void *worker_stack;
        u64 end;

        sample = alloc_shared_region(SHARED_STATE_SIZE);
        if (!sample) {
            return -1;
        }
        worker_stack = alloc_shared_region(WORKER_STACK_SIZE);
        if (!worker_stack) {
            free_shared_region(sample, SHARED_STATE_SIZE);
            return -1;
        }
        sample->remote_cpu = target_cpu;
        sample->affinity = -1;

        if (pthread_attr_init(&attr) != 0) {
            free_shared_region(worker_stack, WORKER_STACK_SIZE);
            free_shared_region(sample, SHARED_STATE_SIZE);
            return -1;
        }
        if (pthread_attr_setstack(&attr, worker_stack, WORKER_STACK_SIZE) != 0
            || pthread_create(&worker, &attr, sched_worker, sample) != 0) {
            pthread_attr_destroy(&attr);
            free_shared_region(worker_stack, WORKER_STACK_SIZE);
            free_shared_region(sample, SHARED_STATE_SIZE);
            return -1;
        }
        pthread_attr_destroy(&attr);
        while (__atomic_load_n(&sample->start_ns, __ATOMIC_ACQUIRE) == 0) {
            usys_yield();
        }
        wait_until_at_least(&sample->done, 1);
        end = mono_ns();

        if (sample->affinity != target_cpu || end < sample->start_ns) {
            printf("[SCHED_NOTIFY_BENCH] ERROR metric=%s sample=%d "
                   "affinity=%d expected=%d start=%lu end=%lu\n",
                   label,
                   i,
                   sample->affinity,
                   target_cpu,
                   sample->start_ns,
                   end);
            return -1;
        }
        if (metric) {
            printf("[SCHED_NOTIFY_BENCH] metric=%s sample=%d latency_ns=%lu\n",
                   metric,
                   i,
                   end - sample->start_ns);
        }
        /* Reap the worker before reusing the sample state. */
        pthread_join(worker, NULL);
        free_shared_region(worker_stack, WORKER_STACK_SIZE);
        free_shared_region(sample, SHARED_STATE_SIZE);
    }
    return 0;
}

static void *notify_waiter(void *opaque)
{
    struct notify_state *state = opaque;
    int i;

    if (usys_set_affinity(-2, state->remote_cpu) != 0) {
        __atomic_store_n(&state->error, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    usys_yield();
    if (usys_get_affinity(-1) != state->remote_cpu) {
        __atomic_store_n(&state->error, 1, __ATOMIC_RELEASE);
        return NULL;
    }

    for (i = 1; i <= state->samples; ++i) {
        __atomic_store_n(&state->ready, i, __ATOMIC_RELEASE);
        if (usys_wait(state->notifc_cap, true, NULL) != 0) {
            __atomic_store_n(&state->error, 1, __ATOMIC_RELEASE);
            return NULL;
        }
        /* This is the endpoint: the notified thread has been selected by the
         * remote scheduler and has resumed execution in user mode. */
        __atomic_store_n(&state->done, i, __ATOMIC_RELEASE);
    }
    return NULL;
}

static int run_notify_samples(int samples, int target_cpu, const char *metric)
{
    struct notify_state *state;
    pthread_t waiter;
    int i;

    state = alloc_shared_region(SHARED_STATE_SIZE);
    if (!state) {
        return -1;
    }
    state->samples = samples;
    state->remote_cpu = target_cpu;
    state->notifc_cap = usys_create_notifc();
    if (state->notifc_cap < 0) {
        return -1;
    }
    if (pthread_create(&waiter, NULL, notify_waiter, state) != 0) {
        return -1;
    }
    for (i = 1; i <= samples; ++i) {
        u64 start;
        u64 notify_end;
        u64 end;
        int ret;

        wait_until_at_least_yielding(&state->ready, i);
        if (__atomic_load_n(&state->error, __ATOMIC_ACQUIRE)) {
            return -1;
        }

        /* ready is published immediately before sys_wait.  Yield once so the
         * waiter is blocked, rather than consuming an early pending notify. */
        usys_yield();
        start = mono_ns();
        do {
            ret = usys_notify(state->notifc_cap);
            if (ret == -EAGAIN) {
                usys_yield();
            }
        } while (ret == -EAGAIN);
        if (ret != 0) {
            return -1;
        }
        notify_end = mono_ns();
        wait_until_at_least(&state->done, i);
        end = mono_ns();

        if (__atomic_load_n(&state->error, __ATOMIC_ACQUIRE)
            || end < start) {
            return -1;
        }
        printf("[SCHED_NOTIFY_BENCH] metric=%s sample=%d latency_ns=%lu "
               "notify_syscall_ns=%lu remote_resume_ns=%lu\n",
               metric,
               i - 1,
               end - start,
               notify_end - start,
               end - notify_end);
    }
    pthread_join(waiter, NULL);
    free_shared_region(state, SHARED_STATE_SIZE);
    return 0;
}

int main(int argc, char **argv)
{
    int samples = argc > 1 ? atoi(argv[1]) : DEFAULT_SAMPLES;
    int local_cpu = argc > 2 ? atoi(argv[2]) : DEFAULT_LOCAL_CPU;
    int remote_cpu = argc > 3 ? atoi(argv[3]) : DEFAULT_REMOTE_CPU;

    if (samples <= 0 || local_cpu <= 0 || remote_cpu < 0
        || local_cpu == remote_cpu) {
        fprintf(stderr,
                "usage: %s [samples] [local-global-cpu] "
                "[cross-machine-global-cpu]\n",
                argv[0]);
        return 2;
    }

    /* Keep the observer in the source VM for both metrics.  Consequently both
     * timestamps use one monotonic-clock domain even though the endpoint runs
     * in another VM. */
    if (usys_set_affinity(-1, 0) != 0) {
        return 1;
    }
    usys_yield();

    printf("[SCHED_NOTIFY_BENCH] BEGIN samples=%d local_cpu=%d "
           "remote_cpu=%d\n",
           samples,
           local_cpu,
           remote_cpu);
    /* Warm each destination before collecting samples. In particular, the
     * cross-machine warm-up establishes the process page table on machine 1;
     * that one-time setup is not scheduler wake-up latency. */
    if (run_sched_samples(1, local_cpu, NULL) != 0
        || run_sched_samples(1, remote_cpu, NULL) != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=sched_warmup\n");
        return 1;
    }
    if (run_sched_samples(samples, local_cpu, "local_sched") != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=local_sched\n");
        return 1;
    }
    if (run_notify_samples(samples, local_cpu, "local_notify") != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=local_notify\n");
        return 1;
    }
    if (run_sched_samples(samples, remote_cpu, "cross_sched") != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=cross_sched\n");
        return 1;
    }
    if (run_notify_samples(samples, remote_cpu, "cross_notify") != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=cross_notify\n");
        return 1;
    }
    printf("[SCHED_NOTIFY_BENCH] DONE\n");
    return 0;
}
