# 死锁问题分析

## 问题现象

1. **Machine 1 和 3**：在 `wait_for_migration_complete` 中等待（持有 `vmspace->pgtbl_lock`）
2. **Machine 0 和 2**：polling 线程卡在 `atomic_compare_exchange_strong_explicit`，等待 `MSG_REQ_READY` 状态
3. **其他机器**：可能也在等待迁移完成

## 死锁原因分析

### 消息流程

```
Machine 1/3 (发送方)                    Machine 0/2 (接收方)
─────────────────────                    ─────────────────────
1. migrate_pages_to_shm()
   └─> mpsc_alloc_msg()
       └─> 设置 msg->state = MSG_REQ_WRITING
   
2. polling_publish_request()
   └─> 设置 msg->state = MSG_REQ_READY  ← 关键步骤
   
3. polling_wait_for_response()
   └─> 等待 msg->state == MSG_RESP_READY
                                       
                                       polling_reader_thread()
                                       └─> 等待 msg->state == MSG_REQ_READY
                                           └─> CAS: MSG_REQ_READY → MSG_RESP_WRITING
                                           └─> handle_polling_request()
                                               └─> handle_polling_kernel_flush_tlb()
                                                   └─> usys_memcpy_and_flush_tlb()
                                           └─> 设置 msg->state = MSG_RESP_READY
```

### 可能的死锁原因

#### 1. **消息状态卡在 MSG_REQ_WRITING**（最可能）

**问题**：Machine 1/3 在 `polling_publish_request` 之前被阻塞或卡住，导致消息状态停留在 `MSG_REQ_READY`，polling 线程永远等不到 `MSG_REQ_READY`。

**可能触发场景**：
- Machine 1/3 在 `migrate_pages_to_shm` → `mpsc_alloc_msg` 中等待（队列满）
- 或者在 `polling_publish_request` 之前发生了页面错误，导致死锁

#### 2. **Polling 线程处理消息时卡住**

**问题**：Machine 0/2 的 polling 线程在处理 `POLLING_KERNEL_REQ_FLUSH_TLB` 时，`usys_memcpy_and_flush_tlb` 内部可能：
- 需要获取某些锁（如 `vmspace->pgtbl_lock`）
- 触发页面错误，导致递归等待
- 等待其他资源

**关键代码**：
```c
// polling_server.c:51-55
int expected = MSG_REQ_READY;
if (atomic_compare_exchange_strong_explicit(&msg->state,
                                            &expected,
                                            MSG_RESP_WRITING,
                                            ...)) {
    // 如果这里卡住，read_index 不会更新
    handle_polling_request(msg);  // ← 可能在这里卡住
}
```

#### 3. **消息队列索引不同步**

**问题**：`write_index` 和 `read_index` 不同步，导致：
- 发送方认为消息已发送（`write_index` 已更新）
- 但接收方的 `read_index` 没有指向正确的消息
- 或者消息状态被错误设置

#### 4. **迁移完成等待的循环依赖**

**问题**：Machine 1/3 等待迁移完成，但迁移需要 Machine 0/2 处理请求，而 Machine 0/2 的 polling 线程可能：
- 在处理迁移请求时，需要访问某些页面
- 这些页面又触发了新的迁移请求
- 形成循环依赖

## 诊断建议

### 1. 检查消息状态

在 gdb 中检查 Machine 0/2 的共享内存区域：

```gdb
# 找到 shm 地址（通常在 dsm_meta->shm_data[0/2].data）
(gdb) p dsm_meta->shm_data[0].data
(gdb) p dsm_meta->shm_data[2].data

# 检查消息状态
(gdb) p ((struct polling_shm_region *)<shm_addr>)->msgs[0].state
(gdb) p ((struct polling_shm_region *)<shm_addr>)->write_index
(gdb) p ((struct polling_shm_region *)<shm_addr>)->read_index

# 检查所有消息的状态
(gdb) set $i=0
(gdb) while $i < MAX_MSG_COUNT
  > p ((struct polling_shm_region *)<shm_addr>)->msgs[$i].state
  > set $i=$i+1
  > end
```

**期望状态**：
- 应该至少有一个消息的状态是 `MSG_REQ_READY`
- 如果所有消息都是 `MSG_FREE` 或 `MSG_REQ_WRITING`，说明消息没有正确发布

### 2. 检查迁移列表

在 Machine 1/3 上检查：

```gdb
# 检查 migrating_va_list
(gdb) p vmspace->migrating_va_list
(gdb) # 遍历列表查看哪些 VA 正在迁移
```

### 3. 检查锁状态

```gdb
# 在 Machine 1/3 上
(gdb) p vmspace->pgtbl_lock
(gdb) p vmspace->vmspace_lock

# 在 Machine 0/2 上（如果 polling 线程卡在 handle_polling_kernel_flush_tlb）
# 检查是否有其他线程持有这些锁
```

### 4. 添加调试输出

在关键位置添加调试输出：

```c
// 在 polling_publish_request 后
kdebug("[MIGRATION] Machine %d: published request, msg_id=%d, state=%d\n", 
       CUR_MACHINE_ID, msg_id, msg->state);

// 在 polling_reader_thread 的 CAS 前后
kdebug("[POLLING] Machine %d: checking msg[%d], state=%d, expected=%d\n",
       my_id, r % MAX_MSG_COUNT, msg->state, MSG_REQ_READY);
```

## 可能的修复方案

### 方案1：确保消息正确发布（最紧急）

检查 `polling_publish_request` 是否被正确调用，确保在 `mpsc_alloc_msg` 后立即调用。

### 方案2：避免在迁移处理中触发新的迁移

在 `handle_polling_kernel_flush_tlb` → `usys_memcpy_and_flush_tlb` 中，确保：
- 不会触发新的页面错误
- 不会需要获取可能被其他线程持有的锁
- 使用预分配的内存，避免动态分配

### 方案3：添加超时机制

在 `polling_wait_for_response` 和 `wait_for_migration_complete` 中添加超时，避免无限等待。

### 方案4：修复消息队列同步

确保 `read_index` 和 `write_index` 的正确同步，可能需要：
- 使用更强的内存屏障
- 检查索引溢出的处理

## 立即检查项

1. **Machine 0/2 的 polling 线程**：是否真的在等待 `MSG_REQ_READY`，还是卡在其他地方？
2. **Machine 1/3**：是否成功调用了 `polling_publish_request`？
3. **消息状态**：实际的消息状态是什么？
4. **队列状态**：`write_index` 和 `read_index` 的值是什么？
