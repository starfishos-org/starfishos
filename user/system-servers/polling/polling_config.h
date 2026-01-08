#pragma once

#define PERF_REAL_READ       false
#define PERF_READ            false
#define PERF_ALLOC_MSG_RETRY false

#define USE_THREAD_POOL       false
#define REUSE_REQ_RESP_BUFFER false

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define POLLING_SHM_SIZE          (PAGE_SIZE * 10UL)
#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)
