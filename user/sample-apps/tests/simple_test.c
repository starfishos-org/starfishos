#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline uint64_t rdtsc() {
  uint64_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return lo | (hi << 32);
}

struct work_arg {
  int thread_id;
  int iter_count;
  int size;
  uint64_t avg_time;
  void (*calculate)(int);
};

void set_cpu(int cpu) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  sched_setaffinity(0, sizeof(mask), &mask);
  sched_yield();
}

__attribute__((optimize("O0"))) void calculate_array(int size) {
  uint64_t arr[size];
  arr[0] = 1;
  arr[1] = 2;
  for (int i = 2; i < size; i++) {
    arr[i] = arr[i - 1] + arr[i - 2];
  }
}

__attribute__((optimize("O0"))) void calculate_readonly(int size) {
  __attribute__((unused)) uint64_t a = 1;
  for (int i = 0; i < size; i++) {
    (void)a;
  }
}

__attribute__((optimize("O0"))) void calculate_swap(int size) {
  __attribute__((unused)) uint64_t a = 1, b = 2;
  for (int i = 0; i < size; i++) {
    a = b;
  }
  return;
}

void *work_func(void *arg) {
  struct work_arg *work_arg = (struct work_arg *)arg;
  set_cpu(work_arg->thread_id);
  uint64_t start = rdtsc();
  for (int i = 0; i < work_arg->iter_count; i++) {
    work_arg->calculate(work_arg->size);
  }
  uint64_t end = rdtsc();
  // printf("Thread %d: Total Time: %lu ( / 1000000) Per Operation: %lu\n",
  //        work_arg->thread_id, (end - start) / 1000000,
  //        (end - start) / work_arg->iter_count);
  work_arg->avg_time = (end - start) / work_arg->iter_count;
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    printf("Usage: %s <iter_count> <size> <num_threads> <calculate_name>\n",
           argv[0]);
    return 1;
  }
  int iter_count = atoi(argv[1]);
  int size = atoi(argv[2]);
  int num_threads = atoi(argv[3]);
  char *calculate_name = argv[4];
  printf("iter_count: %d, size: %d, num_threads: %d, calculate_name: %s\n",
         iter_count, size, num_threads, calculate_name);
  pthread_t thread[num_threads];
  struct work_arg work_arg[num_threads];
  for (int i = 0; i < num_threads; i++) {
    work_arg[i].thread_id = i;
    work_arg[i].iter_count = iter_count;
    work_arg[i].size = size;
    if (strcmp(calculate_name, "array") == 0) {
      work_arg[i].calculate = calculate_array;
    } else if (strcmp(calculate_name, "readonly") == 0) {
      work_arg[i].calculate = calculate_readonly;
    } else if (strcmp(calculate_name, "swap") == 0) {
      work_arg[i].calculate = calculate_swap;
    } else {
      printf("Invalid calculate_name: %s\n", calculate_name);
      return 1;
    }
    pthread_create(&thread[i], NULL, work_func, &work_arg[i]);
  }
  for (int i = 0; i < num_threads; i++) {
    pthread_join(thread[i], NULL);
  }
  uint64_t avg_time = 0;
  for (int i = 0; i < num_threads; i++) {
    avg_time += work_arg[i].avg_time;
  }
  printf("Average Time: %lu\n", avg_time / num_threads);
  return 0;
}
