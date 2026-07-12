#pragma once

#define PERF_REAL_READ       false
#define PERF_READ            true
#define PERF_ALLOC_MSG_RETRY false

#define USE_THREAD_POOL       true
#define REUSE_REQ_RESP_BUFFER false

/* Number of polling threads when USE_THREAD_POOL is true */
/* This directly specifies how many threads will be polling the message queue */
/* For example: 2 means 2 threads will be polling, 0 means no polling threads */
#define NUM_POLLING_THREADS 1

/* CPUs to bind polling threads to */
/* If NUM_POLLING_THREADS > number of CPUs in this list, threads will cycle through */
#define POLLING_CPU_LIST {6, 7, 8, 9}
#define POLLING_CPU_COUNT 4

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define POLLING_SHM_SIZE          (PAGE_SIZE * 64UL)
#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)

/* Print latency statistics every 1000 requests */
#define PRINT_LATENCY_EVERY_1K false
