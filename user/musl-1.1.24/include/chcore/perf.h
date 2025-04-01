#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_PERF_TIME_SIZE 10240
extern volatile bool ipc_perf_enabled;
extern volatile u64 ipc_perf_count_p1;
extern volatile u64 ipc_perf_count_p6;
extern u64 ipc_perf_time_p1[IPC_PERF_TIME_SIZE];
extern u64 ipc_perf_time_p6[IPC_PERF_TIME_SIZE];

static inline u64 rdtsc() {
  u64 hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return lo | (hi << 32);
}

#ifdef __cplusplus
}
#endif
