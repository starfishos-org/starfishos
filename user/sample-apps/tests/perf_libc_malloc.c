/*
 * numa_memory_latency
 * Copyright (c) 2017 UMEZAWA Takeshi
 * This software is licensed under GNU GPL version 2 or later.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pthread.h>
#include <rpmalloc.h>

#define DSM_ENABLED

inline void *mem_malloc (size_t size)
{
    void *temp = rpmalloc (size);
    assert(temp);

    return temp;
}

inline void *mem_malloc_here (size_t size)
{
    void *temp = rpmalloc (size);
    assert(temp);

    return temp;
}

inline void *mem_calloc (size_t num, size_t size)
{
    void *temp = rpcalloc (num, size);
    assert(temp);

    return temp;
}

inline void *mem_realloc (void *ptr, size_t size)
{
    void *temp = rprealloc (ptr, size);
    assert(temp);

    return temp;
}

inline void *mem_memcpy (void *dest, const void *src, size_t size)
{
    return memcpy (dest, src, size);
}

inline void *mem_memset (void *s, int c, size_t n)
{
    return memset (s, c, n);
}

inline void mem_free (void *ptr)
{
    rpfree (ptr);
}

typedef struct {
    size_t size;
    int thread_id;
    int iterations;
    int fixed;
    int low_size;
    int high_size;
    void **allocations;
} thread_data_t;

inline int proc_bind_thread (int cpu_id)
{
    cpu_set_t   cpu_set;

    CPU_ZERO (&cpu_set);
    CPU_SET (cpu_id, &cpu_set);
#if defined DSM_ENABLED
    printf("bind thread to cpu %d\n", cpu_id);
    sched_setaffinity(-2, sizeof(cpu_set), &cpu_set);
#else
    sched_setaffinity (0, sizeof (cpu_set), &cpu_set);
#endif
    return sched_yield();
}

void* allocate_memory(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    proc_bind_thread(data->thread_id);

    void** allocations = data->allocations;

    for (int i = 0; i < data->iterations; i++) {
        size_t size;
        if (data->fixed) {
            size = data->size; // Fixed size
        } else {
            // Random size between low_size and high_size
            size = data->low_size + rand() % (data->high_size - data->low_size + 1);
        }
        allocations[i] = malloc(size);
    }

    return NULL;
}

void free_memory(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    for (int i = 0; i < data->iterations; i++) {
        free(data->allocations[i]);
    }
}

#define NUM_THREADS 8
#define NUM_ALLOCATIONS 100000
#define MODE_FIXED 1
#define MODE_RANDOM 0

int main(int argc, char* argv[])
{
    int c;
    extern char *optarg;

    int thread_num = NUM_THREADS;
    int fixed = MODE_RANDOM;
    int iterations = NUM_ALLOCATIONS;
    int low_size = 8, high_size = 1024;

    while ((c = getopt(argc, argv, "t:m:i:l:h")) != EOF) 
    {
        switch (c) {
            case 't':
                thread_num = atoi(optarg);   
                break;
            case 'f':
                fixed = atoi(optarg);
                break;
            case 'i':
                iterations = atoi(optarg);
                break;
            case 'l':
                low_size = atoi(optarg);
                break;
            case 'h':
                high_size = atoi(optarg);
                break;
            case '?':
                printf("Usage: %s  -t <thread_num> -f <1:fixed,0:random> -i <iterations> -l <low size> -h <high size>\n", argv[0]);
                exit(1);
        }
    }

    printf("Thread Number: %d, Mode: %s, Iterations: %d, Low Size: %d, High Size: %d\n", 
        thread_num, (fixed) ? "Fixed" : "Random", iterations, low_size, high_size);

    pthread_t threads[thread_num];
    thread_data_t thread_data[thread_num];
    clock_t start, end;
    double cpu_time_used;
    long throughput;

    for (int i = 0; i < thread_num; i++) {
        thread_data[i].size = (fixed) ? low_size : 0; // Size is only relevant if fixed
        thread_data[i].thread_id = i;
        thread_data[i].iterations = iterations;
        thread_data[i].fixed = fixed;
        thread_data[i].low_size = low_size;
        thread_data[i].high_size = high_size;
        thread_data[i].allocations = mem_malloc(NUM_ALLOCATIONS * sizeof(void*));
    }

    start = clock();

    for (int i = 0; i < thread_num; i++) {
        pthread_create(&threads[i], NULL, allocate_memory, (void*)&thread_data[i]);
    }

    for (int i = 0; i < thread_num; i++) {
        pthread_join(threads[i], NULL);
    }

    end = clock();
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    throughput = (long)(thread_num * iterations / cpu_time_used);

    printf("Throughput: %ld Op/s\n", throughput);

    for (int i = 0; i < thread_num; i++) {
        mem_free(thread_data[i].allocations);
    }

    return 0;
}
