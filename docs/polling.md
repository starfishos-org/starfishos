### 流程

-- send thread on src machine:
    -- page fault -> call `migrate_pages_to_shm`
        -- malloc page on CXL
        -- publish request w/ `memcpy_and_flush_tlb_on_remote_machine_polling`
        -- busy wait for response
-- polling core on dst machine:
    -- function `sys_memcpy_and_flush_tlb`
        -- flush_tlb_local_and_remote
        -- memcpy
        -- commit_page_to_pmo
        -- flush_tlb_local_and_remote

### 设置参数

- USE_THREAD_POOL：true表示使用线程池，false表示使用单个线程
- NUM_POLLING_THREADS: 设置polling线程数
- POLLING_CPU_LIST: 设置polling线程绑定的CPU列表

### 打印选项

- 发起端打印
    - PGFAULT_STATS_DEBUG
    - 每1000个请求打印一次migration触发page fault的统计信息

- 处理端打印
    - TLB_FLUSH_LATENCY_DEBUG
    - 统计每个具体的处理里面的各环节的占比
