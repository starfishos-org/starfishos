# 设计文档：ChCore 内核 Partial Failure Resilient CXL SHM 分配器

## 1. 背景与目标

### 1.1 问题

ChCore-CXL 的多机 DSM 中，CXL SHM（共享内存区）的当前分配器是标准的 buddy + slab 系统
（`dsm_meta->mem_pool[]` / `dsm_meta->slab_pool[]`）。这套分配器**不能容忍 partial failure**：
若某台 machine 在持有 CXL SHM 对象引用时崩溃，将导致：

- **内存泄漏**：该 machine 分配/引用的对象永不被回收
- **引用计数不一致**：崩溃发生在引用计数更新中途，遗留错误的 ref_cnt
- **悬挂引用**：其他 machine 仍可见一个"已死"机器保留的 offset，访问时行为未定义

### 1.2 参考

- **CXL-SHM 论文**（Zhang et al., 2023）：提出 era-based 非阻塞引用计数 + monitor 外部回收，
  实现了无全局阻塞、无内存泄漏、无 double-free 的 partial failure 容错。
- **开源实现**：`linux-tests/sosp-paper19-ae/`（C++ 用户态）。

本文档设计将其核心机制移植到 ChCore **内核态 C**，作为 `kernel/dsm/dsm_ref_alloc.c` 实现。

### 1.3 设计目标

1. Partial failure 安全：任意 machine 崩溃后，残留的 CXL SHM 对象由 monitor 线程无阻塞回收
2. 非阻塞：alloc/free/ref 操作不持全局锁；仅使用 per-object CAS
3. 可与现有 buddy/slab 共存：作为 CXL SHM 的**专用区域**，不修改现有分配器
4. 适配 ivshmem 模拟环境（无需 clwb 持久化语义，但保留结构以便迁移至真实 CXL）

---

## 2. 核心机制概述

### 2.1 Era-based 引用计数（非阻塞）

每个 CXL SHM 对象的 header 持有一个 64-bit `ref_info` 字段，编码为：

```
 63        48 47      32 31              0
 +-----------+---------+-----------------+
 |  lcid(16) | cnt(16) |    era(32)      |
 +-----------+---------+-----------------+
   last-modifier  ref    last-modifier's
   client ID      count  era number
```

- **lcid**：最后一次修改该字段的 client（machine×cpu）的 ID
- **cnt**：当前引用计数
- **era**：修改时 lcid 方的 era 编号（单调递增计数器）

每个 client 维护一个 `era[MAX_CLIENTS][MAX_CLIENTS]` 矩阵（存放在 CXL SHM）：
`era[i][j]` = "client i 见到的 client j 的最大 era"。
这使得 recovery 可以判断某个 redo log 条目是否已经完成（通过比较 era 值）。

### 2.2 RootRef（TBR，Thread Base Reference）

每次分配同时创建一个 **RootRef**（根引用），存放在 CXL SHM 中该 client 的 TBR 页队列里。
RootRef 是 GC root：持有指向分配对象的 offset。

当 client 崩溃时，monitor 通过扫描其 TBR 页队列，找到所有 GC root，对每个
存活对象递减 ref_cnt，实现级联回收。

### 2.3 Redo Log

每次 CAS 修改 `ref_info` 之前，先向 TLS（存放在 CXL SHM）的 redo 日志区写入操作记录并 flush。
若 client 在 CAS 期间崩溃，monitor 读取 redo 日志，判断操作是否已完成，若未完成则重放（幂等）。

### 2.4 Monitor 线程

一个内核线程（每个 machine 均可运行，或仅 machine 0）周期扫描 TLS 槽：
- 若某槽的 `client_id != 0` 但对应 machine 已死（心跳超时）→ 执行 recovery
- Recovery 步骤：redo_ref_cnt → 回收 TBR → 回收 segment

---

## 3. 内存布局

### 3.1 CXL SHM 整体布局

```
 shm_paddr
 |
 v
 +--------------------+  <- offset 0
 |    dsm_meta_t      |  现有：全局配置、buddy/slab pool（4K 对齐）
 +--------------------+
 |    buddy pages     |  现有 buddy 系统管理的 SHM 页
 +--------------------+
 |  [保留区/空洞]      |
 +--------------------+  <- DSM_REF_REGION_OFFSET（编译时确定，如 shm_size/2）
 |  dsm_ref_meta_t    |  本分配器的 metadata header（4K 对齐）
 +--------------------+
 |  TLS array         |  MAX_CLIENTS × sizeof(dsm_ref_tls_t)
 +--------------------+
 |  Era matrix        |  MAX_CLIENTS × MAX_CLIENTS × 4 bytes
 +--------------------+
 |  Seg alloc state   |  MAX_SEGMENTS × sizeof(dsm_ref_seg_state_t)
 +--------------------+
 |  Segments area     |  MAX_SEGMENTS × SEG_SIZE（每个 segment = 若干 page）
 +--------------------+
```

`DSM_REF_REGION_OFFSET` 在 `kernel/dsm/dsm_config.cmake` 或
`kernel/include/dsm/dsm-single.h` 中以宏定义，确保不与 buddy 管理的区域重叠。

### 3.2 Segment 内部布局

每个 segment（大小 `DSMA_SEG_SIZE = DSMA_PAGES_PER_SEG × DSMA_PAGE_SIZE`）：

```
 segment start
 +---------------------------+
 |  dsm_ref_segment_t header |  包含 thread_id、used、meta_page[] 数组
 |  meta_page[0] .. [N-1]    |  page 元数据（free list head、block_size 等）
 +---------------------------+  <- 实际数据从第 1 页开始（page 0 留给 header）
 |  page 1 data              |  block_size 大小的 block 线性排列
 |  page 2 data              |
 |  ...                      |
 +---------------------------+
```

---

## 4. 核心数据结构（C 内核版）

```c
/* ---- 基本尺寸参数 ---- */
#define DSMA_PAGE_SHIFT       16               /* 64KB per page */
#define DSMA_PAGE_SIZE        (1UL << DSMA_PAGE_SHIFT)
#define DSMA_SEG_SHIFT        (DSMA_PAGE_SHIFT + 7) /* 8MB per segment */
#define DSMA_SEG_SIZE         (1UL << DSMA_SEG_SHIFT)
#define DSMA_SEG_MASK         (DSMA_SEG_SIZE - 1)
#define DSMA_PAGES_PER_SEG    (DSMA_SEG_SIZE / DSMA_PAGE_SIZE)  /* 128 */

#define DSMA_MAX_CLIENTS      256   /* max concurrent machine×cpu clients */
#define DSMA_MAX_SEGMENTS     512
#define DSMA_BIN_SIZE         66    /* size-class bins，与 CXL-SHM 对齐 */
#define DSMA_REDO_CNT         8     /* redo log 条目数 */

/* ---- offset 约定：0 = NULL/无效 ---- */
typedef u64 dsma_off_t;

/* ---- 分配块（block）---- */
struct dsm_ref_block {
    dsma_off_t next;   /* free list: 下一个 free block 的 offset；0=末尾 */
};

/* ---- 对象 header（每个 user allocation 前置） ---- */
struct dsm_ref_obj {
    dsma_off_t  next;            /* free list（回收后使用） */
    volatile u64 ref_info;       /* [lcid:16][ref_cnt:16][era:32] */
    u64          embedded_ref_cnt; /* user data 开头有多少 u64 offset 字段 */
    /* user data 紧随其后 */
};

/* ---- GC root（RootRef / TBR） ---- */
struct __attribute__((aligned(16))) dsm_ref_root {
    dsma_off_t pptr;   /* 指向 dsm_ref_obj 的 offset；0=空 */
    u8         in_use;
    u16        ref_cnt;
};

/* ---- 页元数据 ---- */
struct dsm_ref_page {
    dsma_off_t  free;        /* free block list head（offset） */
    dsma_off_t  local_free;  /* 本线程 local free list */
    u32         block_size;
    u32         used;        /* 已分配 block 数量 */
    dsma_off_t  next;        /* 下一个 page 在队列中的 offset */
    dsma_off_t  prev;
    u8          is_special;  /* 1=TBR page */
};

/* ---- 页队列 ---- */
struct dsm_ref_pq {
    dsma_off_t  first;
    dsma_off_t  last;
    u64         block_size;
};

/* ---- Redo log 条目（占一整个 cache line = 64 bytes） ---- */
#define DSMA_LINK_REF   1
#define DSMA_UNLINK_REF 2

struct dsm_ref_redo {
    u16        func_id;        /* DSMA_LINK_REF / DSMA_UNLINK_REF */
    u16        saved_ref_cnt;
    u32        cur_era;
    dsma_off_t ref_loc;        /* 被更新的指针字段的 offset（存放在 SHM 中） */
    dsma_off_t refed;          /* 目标 dsm_ref_obj 的 offset */
    dsma_off_t old_refed;      /* 备用（UNLINK 前的值） */
    u8         _pad[64 - 5*8]; /* 填充到 64 bytes */
};

/* ---- Thread-local State（TLS，存放在 CXL SHM） ---- */
struct __attribute__((aligned(64))) dsm_ref_tls {
    volatile u64        client_id;          /* 0=free，非零=in use */
    char                redo[64 * DSMA_REDO_CNT]; /* redo log 区，按 cache line 索引 */
    struct dsm_ref_pq   pages[DSMA_BIN_SIZE];    /* size-class page 队列 */
    struct dsm_ref_pq   free_page;               /* 空闲 page 队列 */
};

/* ---- Segment header ---- */
struct dsm_ref_segment {
    u16                thread_id;              /* 拥有该 segment 的 TLS 槽号（1-based）*/
    u32                used;                   /* 已使用 page 数 */
    struct dsm_ref_page meta_page[DSMA_PAGES_PER_SEG];
};

/* ---- Segment Allocation State（SAS）---- */
#define DSMA_SEG_NORMAL         0
#define DSMA_SEG_ABANDON        1
#define DSMA_SEG_POTENTIAL_LEAK 2

struct dsm_ref_seg_state {
    volatile u32 thread_id;   /* CAS：0=free，非0=owning thread */
    u32          ver;
    volatile u64 info;        /* [state:32][thread_free_offset:32] */
};

/* ---- 全局 Metadata（在 CXL SHM 固定偏移处） ---- */
#define DSMA_META_MAGIC 0x44534D41524546ULL  /* "DSMAAREF" */

struct dsm_ref_meta {
    u64        magic;
    dsma_off_t tls_start;       /* TLS array 相对 shm_base 的 offset */
    dsma_off_t era_start;       /* era 矩阵 offset */
    dsma_off_t sas_start;       /* SAS array offset */
    dsma_off_t segments_start;  /* segments area offset */
    u64        total_size;      /* 本分配器管理的总字节数 */
    u64        shm_base_paddr;  /* SHM 物理基址（用于 offset→vaddr 转换） */
} __attribute__((aligned(SIZE_4K)));

/* ---- 分配结果句柄 ---- */
struct dsm_ref_handle {
    dsma_off_t root_offset;  /* RootRef（TBR）的 offset */
    dsma_off_t data_offset;  /* 用户数据区的 offset（dsm_ref_obj 之后） */
};
```

---

## 5. 内存布局常量与 offset 计算

```
DSM_REF_REGION_OFFSET   = 固定值（如 shm_size / 2），写入 dsm-single.h 宏

dsm_ref_meta    at: DSM_REF_REGION_OFFSET
TLS array       at: DSM_REF_REGION_OFFSET + sizeof(dsm_ref_meta)  (align 64)
era matrix      at: TLS_START + MAX_CLIENTS * sizeof(dsm_ref_tls)
SAS array       at: ERA_START + MAX_CLIENTS * MAX_CLIENTS * 4
segments area   at: SAS_START + MAX_SEGMENTS * sizeof(dsm_ref_seg_state)   (align SEG_SIZE)
```

辅助宏（类似原始实现的 `get_data_at_addr`）：

```c
static inline void *dsma_off2ptr(void *shm_base, dsma_off_t off)
{
    return off ? (char *)shm_base + off : NULL;
}
static inline dsma_off_t dsma_ptr2off(void *shm_base, void *ptr)
{
    return ptr ? (char *)ptr - (char *)shm_base : 0;
}
```

---

## 6. ref_info 编解码

```c
static inline u64 dsma_pack_ref_info(u16 lcid, u16 cnt, u32 era)
{
    return ((u64)lcid << 48) | ((u64)cnt << 32) | era;
}
static inline u16  dsma_ref_lcid(u64 ri) { return (ri >> 48) & 0xFFFF; }
static inline u16  dsma_ref_cnt (u64 ri) { return (ri >> 32) & 0xFFFF; }
static inline u32  dsma_ref_era (u64 ri) { return  ri        & 0xFFFFFFFFU; }
```

---

## 7. 关键操作协议

### 7.1 Client 注册：`dsm_ref_thread_init()`

```
1. 构造 client_id = pack(machine_id:16, cpu_id:16, boot_seq:32)
   boot_seq 取自 per-cpu 单调计数，确保重启后 ID 不同。
2. 扫描 TLS 数组，对 client_id=0 的槽执行 CAS(0 → client_id)，成功则获得 tls_id。
3. 初始化 TLS：清空 redo、初始化 pages[] 各队列的 block_size、free_page={0,0,0}。
4. 自增 era[tls_id][tls_id]。
5. 将 tls_id 存入 per-cpu 变量（ChCore: sched_get_current_cpu() 级别的存储）。
```

### 7.2 分配：`dsm_ref_malloc(data_size, embedded_ref_cnt)`

```
1. TBR alloc：
   a. 从 tls->pages[0]（TBR bin，block_size=16）取一个 block
   b. 若 page 无空闲 block → cxl_segment_page_alloc() 获取新 page，初始化 free list
   c. 设置 tbr->in_use=1, tbr->pptr=0, tbr->ref_cnt=0
   d. page->used++; page->free = block->next

2. Data block alloc：
   a. 计算 total = sizeof(dsm_ref_obj) + data_size；查找对应 size bin
   b. 同上从该 bin 取 block
   c. 初始化 obj->ref_info = pack(tls_id, 1, era[tls_id][tls_id])
   d. era[tls_id][tls_id]++
   e. obj->embedded_ref_cnt = embedded_ref_cnt

3. 链接 TBR → obj：
   a. tbr->pptr = dsma_ptr2off(shm_base, obj)
   b. tbr->ref_cnt = 1
   （此处可插入 fence，在真实 CXL 上保证写入顺序）

4. 返回 {dsma_ptr2off(shm_base, tbr), dsma_ptr2off(shm_base, obj+1)}
```

### 7.3 引用建立：`dsm_ref_link(u64 *ref_loc_in_shm, dsma_off_t target)`

```
ptr_target = dsma_off2ptr(shm_base, target) 指向 dsm_ref_obj

loop {
    ri = atomic_load(&ptr_target->ref_info)
    saw_lcid = dsma_ref_lcid(ri)
    saw_era  = dsma_ref_era(ri)
    cur_era  = era[tls_id][tls_id]

    // era table 更新
    if saw_era > era[tls_id][saw_lcid]:
        era[tls_id][saw_lcid] = saw_era
        // 真实 CXL 上：clwb + sfence

    // 写 redo log（循环覆盖，以 cur_era mod DSMA_REDO_CNT 为槽号）
    redo_slot = &tls->redo[64 * (cur_era & (DSMA_REDO_CNT-1))]
    *redo_slot = {DSMA_LINK_REF, cnt, cur_era,
                  dsma_ptr2off(shm_base, ref_loc_in_shm), target}
    // 真实 CXL 上：clwb(redo_slot) + sfence

    new_ri = dsma_pack_ref_info(tls_id, cnt+1, cur_era)
    if CAS(&ptr_target->ref_info, ri, new_ri): break
}

*ref_loc_in_shm = target    // 写指针
era[tls_id][tls_id]++
```

### 7.4 引用释放：`dsm_ref_unlink(u64 *ref_loc_in_shm, dsma_off_t target)`

```
与 link 类似，CAS 将 cnt 减 1，new_ri = pack(tls_id, cnt-1, cur_era)

*ref_loc_in_shm = 0
era[tls_id][tls_id]++

if cnt-1 == 0:
    dsm_ref_block_free(ptr_target)  // 将 block 归还到 page free list
```

### 7.5 Block 释放：`dsm_ref_block_free(block, special=false)`

```
page    = 通过 block 地址对齐到 SEG_SIZE 找到 segment，再找到 page
segment = block & ~SEG_MASK

memset(block, 0, block_size)

if segment->thread_id == cur_tls_id:
    // 本地释放：直接入 local_free list
    block->next = page->local_free
    page->local_free = dsma_ptr2off(shm_base, block)
    page->used--
    if page->used == 0: dsm_ref_page_free(page)
else:
    // 跨线程释放：入 segment 的 thread_free 链（lock-free CAS）
    sas = sas_array[seg_index]
    do {
        info = load(&sas->info)
        if info.state == POTENTIAL_LEAK: break  // 交给 monitor
        block->next = info.thread_free_offset
        new_info = {info.state, dsma_ptr2off(shm_base, block)}
    } while !CAS(&sas->info, info, new_info)
```

---

## 8. Recovery 机制

### 8.1 活性检测（心跳）

在 `dsm_meta` 中新增：
```c
volatile u64 heartbeat[CLUSTER_MAX_MACHINE_NUM]; /* 每台 machine 定期更新 */
u64 heartbeat_threshold_ms;                       /* 超过此时间无更新视为死亡 */
```

每台 machine 的定时器 handler 写：`dsm_meta->heartbeat[CUR_MACHINE_ID]++`

Monitor 检查：`current_ticks - last_seen_heartbeat[mid] > threshold` → machine mid 已死。

### 8.2 Redo 重放：`dsm_ref_redo_ref_cnt(tls_offset)`

```
tls = dsma_off2ptr(shm_base, tls_offset)
tls_id = (tls_offset - tls_start) / sizeof(dsm_ref_tls) + 1

// 找最新的 redo log 条目（cur_era 最大）
best_redo = max(tls->redo[0..DSMA_REDO_CNT-1]) by cur_era

if best_redo->func_id == DSMA_LINK_REF:
    obj = dsma_off2ptr(shm_base, best_redo->refed)
    ri  = obj->ref_info
    // 判断此操作是否已完成：
    // 若 obj 的 lcid == dead tls_id 且 era == best_redo->cur_era → 未完成
    need_redo = false
    if dsma_ref_lcid(ri)==tls_id && dsma_ref_era(ri)==best_redo->cur_era:
        need_redo = true
    else:
        // 检查全局 era 表：若 best_redo->cur_era <= 其他 client 见到的 dead 方 era → 已完成
        max_era_seen = max(era[j][tls_id] for j != tls_id)
        if best_redo->cur_era <= max_era_seen: need_redo = true
    if need_redo:
        *dsma_off2ptr(shm_base, best_redo->ref_loc) = best_redo->refed

if best_redo->func_id == DSMA_UNLINK_REF:
    // 类似逻辑，重放结果是写 0
    if need_redo:
        *dsma_off2ptr(shm_base, best_redo->ref_loc) = 0
    // 若 saved_ref_cnt == 1，标记 segment 为 POTENTIAL_LEAK
    if best_redo->saved_ref_cnt == 1:
        sas->info.state = DSMA_SEG_POTENTIAL_LEAK (via CAS)
```

### 8.3 GC 回收：`dsm_ref_recover_client(tls_offset, dead_client_id)`

```
步骤 1：redo_ref_cnt(tls_offset)

步骤 2：回收所有 TBR
  for each page in tls->pages[0] (TBR bin):
    for each block in page:
      tbr = (dsm_ref_root*)block
      if tbr->in_use == 1:
        if tbr->pptr != 0:
          recovery_test_free(tbr->pptr, tls_offset)
        tbr->in_use = 0
        recovery_cxl_free(true, block, tls_offset)
    free page metadata

步骤 3：回收数据 page（pages[2..BIN_SIZE]）
  for each bin i in 2..BIN_SIZE:
    for each page in tls->pages[i]:
      collect local_free (merge into free list)
      if page->used == 0:
        dsm_ref_page_free(page)
      else:
        // 有存活 block → 此 segment 交给 monitor 扫描
        sas->info.state = DSMA_SEG_ABANDON (via CAS)

步骤 4：释放仍归属于 dead tls_id 且未 ABANDON 的 segments
  for i in 0..MAX_SEGMENTS:
    sas = sas_array[i]
    if sas->thread_id == tls_id && sas->info.state != ABANDON:
      dsm_ref_segment_free(segment_i)

步骤 5：清空 TLS 槽
  tls->pages = reset to initial state
  tls->free_page = {0,0,0}
  CAS(&tls->client_id, dead_client_id, 0)
```

**recovery_test_free(ref_offset, tls_offset)**：递归回收
```
obj = dsma_off2ptr(shm_base, ref_offset)
ri  = obj->ref_info
if ref_cnt(ri) == 0: return
if ref_cnt(ri) > 1:
    recovery_unlink_reference(ref_offset, ref_offset, tls_offset)
    return
// ref_cnt == 1：需要递归回收 embedded refs
for i in 0..obj->embedded_ref_cnt-1:
    embedded_ref = (u64*)(obj+1) + i
    recovery_test_free(*embedded_ref, tls_offset)
recovery_unlink_reference(ref_offset, ref_offset, tls_offset)
```

### 8.4 Monitor 主循环：`dsm_ref_monitor_loop()`

```c
void dsm_ref_monitor_loop(void)
{
    while (!should_stop) {
        /* 1. 检查所有 TLS 槽 */
        for (i = 0; i < DSMA_MAX_CLIENTS; i++) {
            tls_off = tls_start + i * sizeof(dsm_ref_tls_t);
            id = atomic_load(&tls[i].client_id);
            if (id != 0 && dsm_ref_client_is_dead(id)) {
                dsm_ref_redo_ref_cnt(tls_off);
                dsm_ref_recover_client(tls_off, id);
            }
        }

        /* 2. 扫描 ABANDON/POTENTIAL_LEAK 的 segment */
        for (i = 0; i < DSMA_MAX_SEGMENTS; i++) {
            sas = &sas_array[i];
            if (sas->thread_id == 0) continue;
            if (sas->info.state == ABANDON || POTENTIAL_LEAK) {
                dsm_ref_scan_and_reclaim_segment(i);
            }
        }

        usleep_range(MONITOR_INTERVAL_US, MONITOR_INTERVAL_US * 2);
    }
}
```

---

## 9. Size Class 方案

与 CXL-SHM 一致，以 16 字节为粒度：

| Bin 索引 | block_size        | 用途                              |
|----------|-------------------|-----------------------------------|
| 0        | 16                | TBR（RootRef，16 bytes）          |
| 1        | 对齐后的 msg_queue_size | 消息队列（可暂不实现）       |
| 2        | 16                | 一般数据                          |
| 3        | 32                | 一般数据                          |
| i (i≥2)  | 16×(i-1)          | 一般数据                          |
| 65       | 16×64 = 1024      | 最大 size class（> 1024 走 buddy）|

对于超过最大 size class 的分配，退回到现有 CXL buddy/slab 分配器（不带 partial failure 保护）。

---

## 10. 与 CXL-SHM 原始实现的对比

| 方面               | CXL-SHM（用户态 C++）          | ChCore 内核移植（C）               |
|--------------------|--------------------------------|------------------------------------|
| Client 标识        | MAC address(48bit) + PID(16bit) | `(machine_id:16)(cpu_id:16)(seq:32)` |
| 原子操作           | `std::atomic<>` / CAS          | `cmpxchg64()` / `atomic_load/store` |
| 内存屏障           | `FENCE`（x86 sfence）          | `smp_mb()` / x86 `mfence`          |
| 持久化 flush       | `clwb`（真实 CXL 需要）        | 真实 CXL：保留 clwb；ivshmem 模拟：no-op |
| 线程初始化标识     | `get_mac() + gettid()`         | `CUR_MACHINE_ID + smp_processor_id()` |
| Monitor 进程       | 独立用户进程                   | `kthread_run()` 内核线程           |
| 活性检测           | `kill(pid, 0)` 检查 PID        | `dsm_meta->heartbeat[]` 心跳       |
| RAII               | `CXLRef` 析构函数              | 显式 `dsm_ref_link/unlink` 调用    |
| SHM 映射           | posix shm / CXL DAX mmap       | ivshmem 已映射；`phys_to_virt(shm_paddr)` |
| C++ 容器           | `std::atomic<state_free_info>` | 拆分为 `volatile u32 state` + `volatile u32 thread_free_offset` in `volatile u64 info` |

---

## 11. 与 ChCore DSM 的集成点

### 11.1 初始化（`kernel/dsm/dsm_ref_alloc.c`）

```c
/* Machine 0 在 dsm_init_mm() 之后调用 */
void dsm_ref_alloc_global_init(void)
{
    void *shm_base = phys_to_virt(dsm_meta->shm_paddr);
    u64   region_off = DSM_REF_REGION_OFFSET;  /* 宏定义 */
    dsm_ref_alloc_init(shm_base, dsm_meta->shm_size, region_off);
}

/* 每台 machine boot 时，为每个 CPU 调用 */
void dsm_ref_cpu_init(void)
{
    dsm_ref_thread_init();  /* 注册 TLS 槽 */
}
```

### 11.2 dsm_meta 补充字段

在 `dsm_metadata_t` 结构末尾添加：

```c
/* 8. resilient allocator 指针（初始化后设置） */
struct dsm_ref_meta *ref_alloc_meta;

/* 9. heartbeat for partial failure detection */
volatile u64 heartbeat[CLUSTER_MAX_MACHINE_NUM];
```

### 11.3 Monitor 线程启动

在 `dsm_add_machine()`（machine 0）之后：

```c
kthread_run(dsm_ref_monitor_loop, NULL, "dsm-ref-monitor");
```

### 11.4 现有代码不受影响

- buddy/slab 分配（`dsm_meta->mem_pool[]`）继续服务现有调用路径
- 新的 resilient allocator 作为**可选增强**：需要 partial failure 保护的对象改用 `dsm_ref_malloc/link/unlink`
- 文件路径：
  - `kernel/dsm/dsm_ref_alloc.c` — 分配器主体
  - `kernel/dsm/dsm_ref_recovery.c` — recovery/monitor 逻辑
  - `kernel/include/dsm/dsm_ref_alloc.h` — 对外 API

---

## 12. 实现阶段规划

### 阶段 1：基础分配器（无 recovery）
- `dsm_ref_alloc_init()`：初始化 SHM 区域各区
- `dsm_ref_thread_init/exit()`：TLS 注册/注销（注销时做 normal GC）
- `dsm_ref_malloc() / dsm_ref_block_free()`：segment→page→block 层次分配
- 正确性测试：多核 alloc/free，检查内存不泄漏

### 阶段 2：引用计数 + Redo Log
- `dsm_ref_link() / dsm_ref_unlink()`
- Era 矩阵维护
- Redo log 写入与 flush
- 测试：fault injection（在 redo 写入后、CAS 前 kill）+ 手动 recovery

### 阶段 3：Monitor + Recovery
- 心跳机制
- `dsm_ref_redo_ref_cnt()` / `dsm_ref_recover_client()`
- Monitor 内核线程
- 测试：模拟 machine crash，验证 monitor 能回收所有对象，无 double-free

### 阶段 4：集成到 DSM 关键路径
- 将 DSM 共享队列、跨 machine IPC 对象等改用 resilient allocator
- 与 GeminiGraph/pagerank 工作负载联调

---

## 13. 局限与已知问题

1. **QEMU ivshmem 无持久化**：clwb/sfence 在 ivshmem 模拟下无语义；但协议本身仍保证
   crash recovery 的正确性（依赖 era 逻辑，不依赖持久化顺序）。

2. **Era 溢出**：era 为 32-bit，在极高频操作下理论上可能溢出（约 4 billion 次操作后）。
   实际 DSM 工作负载下不太可能触达，可在后续版本改为 64-bit。

3. **MAX_CLIENTS 限制**：256 个 client（machine × cpu 粒度）。4 机器 × 64 CPU = 256，
   恰好在边界。若 CPU 数更多，需调整 `DSMA_MAX_CLIENTS`（会影响 era 矩阵内存占用：
   256×256×4 = 256KB）。

4. **Embedded ref 递归回收深度**：recovery_test_free 是递归调用，深度取决于对象图深度。
   内核栈有限，实际使用时建议对象图深度不超过 16 层，或改为显式栈实现。

5. **ABA 问题（hash index 区）**：原 CXL-SHM 的 hash index 有未解决的 ABA 问题（代码注释
   中有 TODO）。本移植暂不实现 hash index，如有需要另行设计。
