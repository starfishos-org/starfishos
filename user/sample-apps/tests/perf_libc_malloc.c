/*
 * numa_memory_latency
 * Copyright (c) 2017 UMEZAWA Takeshi
 * This software is licensed under GNU GPL version 2 or later.
 */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <rpmalloc.h>

#define DSM_ENABLED

int proc_bind_thread(int cpu_id) {
  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);
#if defined DSM_ENABLED
  sched_setaffinity(-2, sizeof(cpu_set), &cpu_set);
#else
  sched_setaffinity(0, sizeof(cpu_set), &cpu_set);
#endif
  return sched_yield();
}

int proc_rebind_thread(int cpu_id) {
  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);
#if defined DSM_ENABLED
  // printf("rebind thread to cpu %d\n", cpu_id);
  sched_setaffinity(-2, sizeof(cpu_set), &cpu_set);
#else
  sched_setaffinity(0, sizeof(cpu_set), &cpu_set);
#endif
  return sched_yield();
}

inline void *mem_malloc(size_t size) {
  void *temp = malloc(size);
  assert(temp);

  return temp;
}

inline void *mem_malloc_here(size_t size) {
  void *temp = malloc(size);
  assert(temp);

  return temp;
}

inline void *mem_calloc(size_t num, size_t size) {
  void *temp = rpcalloc(num, size);
  assert(temp);

  return temp;
}

inline void *mem_realloc(void *ptr, size_t size) {
  void *temp = rprealloc(ptr, size);
  assert(temp);

  return temp;
}

inline void *mem_memcpy(void *dest, const void *src, size_t size) {
  return memcpy(dest, src, size);
}

inline void *mem_memset(void *s, int c, size_t n) { return memset(s, c, n); }

inline void mem_free(void *ptr) { free(ptr); }

typedef struct {
  size_t size;
  int thread_id;
  int iterations;
  int fixed;
  int low_size;
  int high_size;
  void **allocations;
} thread_data_t;

double *run_times;
size_t *alloc_sizes;

void *allocate_memory(void *arg) {

  thread_data_t *data = (thread_data_t *)arg;
  proc_bind_thread(data->thread_id);

  void **allocations = data->allocations;
  struct timespec start, end;
  double cpu_time_used;

  clock_gettime(CLOCK_MONOTONIC, &start);

  for (int i = 0; i < data->iterations; i++) {
    size_t size;
    if (data->fixed) {
      size = data->size;  // Fixed size
    } else {
      // Random size between low_size and high_size
      size = alloc_sizes[i];
    }
    if (i % (data->iterations / 10) == 0) {
      printf("Thread %d: %d data %lu\n", data->thread_id, i, size);
    }
    allocations[i] = mem_malloc(size);
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  cpu_time_used =
      (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
  printf("Thread %d: %f seconds\n", data->thread_id, cpu_time_used);
  run_times[data->thread_id] = cpu_time_used;

  proc_rebind_thread(data->thread_id % 48);

  return NULL;
}

#define NUM_THREADS 8
#define NUM_ALLOCATIONS 100000
#define MODE_FIXED 1
#define MODE_RANDOM 0

int main(int argc, char *argv[]) {
  int c;
  extern char *optarg;

  int thread_num = NUM_THREADS;
  int fixed = MODE_RANDOM;
  int iterations = NUM_ALLOCATIONS;
  int low_size = 8, high_size = 1024;

  while ((c = getopt(argc, argv, "t:f:i:l:h")) != EOF) {
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
        printf(
            "Usage: %s  -t <thread_num> -f <1:fixed,0:random> -i <iterations> "
            "-l <low size> -h <high size>\n",
            argv[0]);
        exit(1);
    }
  }

  printf(
      "Thread Number: %d, Mode: %s, Iterations: %d, Low Size: %d, High Size: "
      "%d\n",
      thread_num, (fixed) ? "Fixed" : "Random", iterations, low_size,
      high_size);

  proc_rebind_thread(0);

  pthread_t threads[thread_num];
  thread_data_t thread_data[thread_num];
  double cpu_time_used = 0;
  long throughput;
  int number;

  run_times = malloc(thread_num * sizeof(double));
  if (!fixed) {
    alloc_sizes = malloc(iterations * sizeof(size_t));

    FILE *file = fopen("mem_alloc.txt", "r");
    if (file == NULL) {
      printf("Failed to open file");
      return EXIT_FAILURE;
    }

    for (int i = 0; i < iterations; i++) {
      if (fscanf(file, "%d", &number) == 1) {
        alloc_sizes[i] = number;
      } else {
        alloc_sizes[i] = low_size + (rand() % (high_size - low_size + 1));
      }
    }
  }

  for (int i = 0; i < thread_num; i++) {
    thread_data[i].size =
        (fixed) ? low_size : 0;  // Size is only relevant if fixed
    thread_data[i].thread_id = i;
    thread_data[i].iterations = iterations;
    thread_data[i].fixed = fixed;
    thread_data[i].low_size = low_size;
    thread_data[i].high_size = high_size;
    thread_data[i].allocations = mem_malloc(iterations * sizeof(void *));
  }

  for (int i = 0; i < thread_num; i++) {
    pthread_create(&threads[i], NULL, allocate_memory, (void *)&thread_data[i]);
  }

  for (int i = 0; i < thread_num; i++) {
    pthread_join(threads[i], NULL);
  }

  for (int i = 0; i < thread_num; i++) {
    cpu_time_used += run_times[i];
  }

  cpu_time_used /= thread_num;
  throughput = (long)(thread_num * iterations / cpu_time_used);

  printf("Throughput: %ld Op/s\n", throughput);

  for (int i = 0; i < thread_num; i++) {
    mem_free(thread_data[i].allocations);
  }

  free(run_times);
  if (!fixed) {
    free(alloc_sizes);
  }

  rpmalloc_finalize();

  return 0;
}
