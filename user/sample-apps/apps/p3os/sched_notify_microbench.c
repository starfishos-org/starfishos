#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <chcore/syscall.h>
#include <chcore/type.h>

#define DEFAULT_SAMPLES 50
#define DEFAULT_REMOTE_CPU 12

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

static int run_sched_samples(int samples, int remote_cpu)
{
    int i;

    for (i = 0; i < samples; ++i) {
        struct sched_sample *sample;
        pthread_t worker;
        u64 end;

        sample = calloc(1, sizeof(*sample));
        if (!sample) {
            return -1;
        }
        sample->remote_cpu = remote_cpu;
        sample->affinity = -1;

        if (pthread_create(&worker, NULL, sched_worker, sample) != 0) {
            return -1;
        }
        while (__atomic_load_n(&sample->start_ns, __ATOMIC_ACQUIRE) == 0) {
            usys_yield();
        }
        wait_until_at_least(&sample->done, 1);
        end = mono_ns();

        if (sample->affinity != remote_cpu || end < sample->start_ns) {
            printf("[SCHED_NOTIFY_BENCH] ERROR metric=sched sample=%d "
                   "affinity=%d expected=%d start=%lu end=%lu\n",
                   i,
                   sample->affinity,
                   remote_cpu,
                   sample->start_ns,
                   end);
            return -1;
        }
        printf("[SCHED_NOTIFY_BENCH] metric=sched sample=%d latency_ns=%lu\n",
               i,
               end - sample->start_ns);
        /* Reap the worker before reusing the sample state. */
        pthread_join(worker, NULL);
        free(sample);
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

static int run_notify_samples(int samples, int remote_cpu)
{
    struct notify_state *state;
    pthread_t waiter;
    int i;

    state = calloc(1, sizeof(*state));
    if (!state) {
        return -1;
    }
    state->samples = samples;
    state->remote_cpu = remote_cpu;
    state->notifc_cap = usys_create_notifc();
    if (state->notifc_cap < 0) {
        return -1;
    }
    if (pthread_create(&waiter, NULL, notify_waiter, state) != 0) {
        return -1;
    }
    for (i = 1; i <= samples; ++i) {
        u64 start;
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
        wait_until_at_least(&state->done, i);
        end = mono_ns();

        if (__atomic_load_n(&state->error, __ATOMIC_ACQUIRE)
            || end < start) {
            return -1;
        }
        printf("[SCHED_NOTIFY_BENCH] metric=notify sample=%d latency_ns=%lu\n",
               i - 1,
               end - start);
    }
    pthread_join(waiter, NULL);
    free(state);
    return 0;
}

int main(int argc, char **argv)
{
    int samples = argc > 1 ? atoi(argv[1]) : DEFAULT_SAMPLES;
    int remote_cpu = argc > 2 ? atoi(argv[2]) : DEFAULT_REMOTE_CPU;

    if (samples <= 0 || remote_cpu < 0) {
        fprintf(stderr, "usage: %s [samples] [remote-global-cpu]\n", argv[0]);
        return 2;
    }

    /* Keep the observer in the source VM for both metrics.  Consequently both
     * timestamps use one monotonic-clock domain even though the endpoint runs
     * in another VM. */
    if (usys_set_affinity(-1, 0) != 0) {
        return 1;
    }
    usys_yield();

    printf("[SCHED_NOTIFY_BENCH] BEGIN samples=%d remote_cpu=%d\n",
           samples,
           remote_cpu);
    if (run_sched_samples(samples, remote_cpu) != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=sched\n");
        return 1;
    }
    if (run_notify_samples(samples, remote_cpu) != 0) {
        printf("[SCHED_NOTIFY_BENCH] FAILED phase=notify\n");
        return 1;
    }
    printf("[SCHED_NOTIFY_BENCH] DONE\n");
    return 0;
}
