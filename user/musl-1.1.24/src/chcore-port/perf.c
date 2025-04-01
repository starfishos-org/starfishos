#include <chcore/perf.h>
#include <chcore/syscall.h>
#include <stdio.h>

volatile bool ipc_perf_enabled = false;
volatile u64 ipc_perf_count_p1 = 0;
volatile u64 ipc_perf_count_p6 = 0;
u64 ipc_perf_time_p1[IPC_PERF_TIME_SIZE];
u64 ipc_perf_time_p6[IPC_PERF_TIME_SIZE];
