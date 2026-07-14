#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SAMPLES 1000

struct sched_sample {
    atomic_uint_fast64_t start_ns;
    atomic_int done;
    atomic_int observed_cpu;
    int remote_cpu;
};

struct notify_state {
    atomic_int ready;
    atomic_int done;
    atomic_int error;
    int samples;
    int remote_cpu;
    int event_fd;
};

static uint64_t mono_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int pin_current_thread(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static void wait_until_at_least(atomic_int *value, int expected)
{
    unsigned int spins = 0;

    while (atomic_load_explicit(value, memory_order_acquire) < expected) {
        if ((++spins & 0xfffffU) == 0)
            sched_yield();
    }
}

static void *sched_worker(void *opaque)
{
    struct sched_sample *sample = opaque;
    uint64_t start = mono_ns();

    atomic_store_explicit(&sample->start_ns, start, memory_order_release);
    if (pin_current_thread(sample->remote_cpu) != 0) {
        atomic_store_explicit(&sample->observed_cpu, -errno,
                              memory_order_release);
        atomic_store_explicit(&sample->done, 1, memory_order_release);
        return NULL;
    }

    /* Match the ChCore test: the endpoint is reached only after the migrating
     * thread has resumed in user mode on the destination CPU. */
    sched_yield();
    atomic_store_explicit(&sample->observed_cpu, sched_getcpu(),
                          memory_order_release);
    atomic_store_explicit(&sample->done, 1, memory_order_release);
    return NULL;
}

static int run_sched_samples(int samples, int remote_cpu)
{
    int i;

    for (i = 0; i < samples; ++i) {
        struct sched_sample sample;
        pthread_t worker;
        uint64_t start, end;
        int observed_cpu;

        atomic_init(&sample.start_ns, 0);
        atomic_init(&sample.done, 0);
        atomic_init(&sample.observed_cpu, -1);
        sample.remote_cpu = remote_cpu;

        if (pthread_create(&worker, NULL, sched_worker, &sample) != 0) {
            perror("pthread_create");
            return -1;
        }
        while ((start = atomic_load_explicit(&sample.start_ns,
                                              memory_order_acquire)) == 0)
            sched_yield();
        wait_until_at_least(&sample.done, 1);
        end = mono_ns();
        observed_cpu = atomic_load_explicit(&sample.observed_cpu,
                                             memory_order_acquire);
        pthread_join(worker, NULL);

        if (observed_cpu != remote_cpu || end < start) {
            fprintf(stderr,
                    "sched sample %d failed: observed_cpu=%d expected=%d\n",
                    i, observed_cpu, remote_cpu);
            return -1;
        }
        printf("[SCHED_NOTIFY_BENCH] metric=sched sample=%d latency_ns=%llu\n",
               i, (unsigned long long)(end - start));
    }
    return 0;
}

static void *notify_waiter(void *opaque)
{
    struct notify_state *state = opaque;
    uint64_t value;
    int i;

    if (pin_current_thread(state->remote_cpu) != 0) {
        atomic_store_explicit(&state->error, errno, memory_order_release);
        return NULL;
    }

    for (i = 1; i <= state->samples; ++i) {
        atomic_store_explicit(&state->ready, i, memory_order_release);
        do {
            errno = 0;
        } while (read(state->event_fd, &value, sizeof(value)) < 0
                 && errno == EINTR);
        if (errno != 0) {
            atomic_store_explicit(&state->error, errno,
                                  memory_order_release);
            return NULL;
        }
        atomic_store_explicit(&state->done, i, memory_order_release);
    }
    return NULL;
}

static int run_notify_samples(int samples, int remote_cpu)
{
    struct notify_state state;
    pthread_t waiter;
    uint64_t one = 1;
    int i;

    atomic_init(&state.ready, 0);
    atomic_init(&state.done, 0);
    atomic_init(&state.error, 0);
    state.samples = samples;
    state.remote_cpu = remote_cpu;
    state.event_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
    if (state.event_fd < 0) {
        perror("eventfd");
        return -1;
    }
    if (pthread_create(&waiter, NULL, notify_waiter, &state) != 0) {
        perror("pthread_create");
        close(state.event_fd);
        return -1;
    }

    for (i = 1; i <= samples; ++i) {
        uint64_t start, end;

        while (atomic_load_explicit(&state.ready, memory_order_acquire) < i) {
            if (atomic_load_explicit(&state.error, memory_order_acquire))
                goto fail;
            sched_yield();
        }

        /* Encourage the waiter to enter eventfd_read().  An early write is
         * still correct because eventfd, like ChCore notification, retains a
         * pending notification. */
        sched_yield();
        start = mono_ns();
        if (write(state.event_fd, &one, sizeof(one)) != sizeof(one)) {
            perror("eventfd write");
            goto fail;
        }
        wait_until_at_least(&state.done, i);
        end = mono_ns();
        if (atomic_load_explicit(&state.error, memory_order_acquire)
            || end < start)
            goto fail;

        printf("[SCHED_NOTIFY_BENCH] metric=notify sample=%d latency_ns=%llu\n",
               i - 1, (unsigned long long)(end - start));
    }

    pthread_join(waiter, NULL);
    close(state.event_fd);
    return 0;

fail:
    pthread_cancel(waiter);
    pthread_join(waiter, NULL);
    close(state.event_fd);
    return -1;
}

static int find_default_cpus(int *source_cpu, int *remote_cpu)
{
    cpu_set_t allowed;
    int cpu;

    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return -1;
    *source_cpu = -1;
    *remote_cpu = -1;
    for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed))
            continue;
        if (*source_cpu < 0)
            *source_cpu = cpu;
        else {
            *remote_cpu = cpu;
            break;
        }
    }
    return *source_cpu >= 0 && *remote_cpu >= 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    int samples = argc > 1 ? atoi(argv[1]) : DEFAULT_SAMPLES;
    int source_cpu, remote_cpu;

    if (find_default_cpus(&source_cpu, &remote_cpu) != 0) {
        fprintf(stderr, "at least two CPUs must be available\n");
        return 2;
    }
    if (argc > 2)
        source_cpu = atoi(argv[2]);
    if (argc > 3)
        remote_cpu = atoi(argv[3]);
    if (samples <= 0 || source_cpu < 0 || remote_cpu < 0
        || source_cpu == remote_cpu) {
        fprintf(stderr, "usage: %s [samples] [source-cpu] [remote-cpu]\n",
                argv[0]);
        return 2;
    }
    if (pin_current_thread(source_cpu) != 0) {
        perror("pin source CPU");
        return 2;
    }

    printf("[SCHED_NOTIFY_BENCH] BEGIN platform=linux samples=%d "
           "source_cpu=%d remote_cpu=%d\n",
           samples, source_cpu, remote_cpu);
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
