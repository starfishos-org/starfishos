# 为什么「划分合理」时 90s，比全在 CXL 还慢约 30s？

## 现象

- **exec_time ≈ 91.6s**（当前 log：划分按 partition 绑到 socket，tune_chunks 看起来合理）
- **全量数据在 CXL 时约 60s**
- 即：数据按 partition 落在各机本地 DRAM 时，反而比「全在 CXL」慢约 **30s**。

## 根本原因：划分只决定「谁算」，不决定「谁访问」

### 1. 划分在做什么

- Partition 0 → machine 0（socket 0）的 8 个线程算；partition 1 → machine 1（socket 1）的 8 个线程算。
- `curr`/`next` 等通过 `fill_vertex_array` 按 partition 首次接触，页落在各自机器本地 DRAM。
- **所以「谁负责算哪一块」和「数据放在哪」是对齐的。**

### 2. process_edges 的访问模式是「全图读 curr、全图写 next」

Pagerank 的 `process_edges` 里（dense 阶段）：

- **读**：对每个 dst，遍历其 **入边**，做 `sum += curr[src]`。
  - `src` 是**全局顶点**，可以属于 **任意 partition**。
  - 因此 machine 1 在算 partition 1 的 dst 时，会大量读 **partition 0 的 curr**（即 machine 0 的 DRAM）。
- **写**：`write_add(&next[dst], msg)`，dst 也是全图，所以 machine 0 会写 partition 1 的 next，machine 1 会写 partition 0 的 next。

也就是说：**计算任务按 partition 划分了，但访问的 VA 仍是全图的；大量访问落在「另一台机器的本地 DRAM」上。**

### 3. 访问「对方机器 DRAM」时发生什么（DSM case 2）

当 machine 1 访问一个当前在 machine 0 本地 DRAM 的页时：

1. **Page fault**（该 VA 在 M1 上无有效映射或为 migration entry）
2. **DSM 迁移**：该页从 M0 拷到 **CXL 共享区**，并在 M1 的页表里映射到 CXL 上的这份拷贝
3. 每次迁移伴随：
   - 跨机通信（ivshmem 门铃 / 消息）
   - 约 4KB 拷贝
   - 源端 TLB shootdown（可能 IPI）
   - 锁竞争（如 `dsm_meta`、页表等）

**curr** 约 41652230 × 8 字节 ≈ 333MB，约 8 万页。即使只有一半被 M1 在 process_edges 里碰到，也会触发数万次迁移，每次迁移成本远高于一次本地 DRAM 或 CXL 访问，累积成 **几十秒** 是合理的。

### 4. 和「全在 CXL」对比

| 场景           | 数据位置     | 访问方式           | 主要成本                     |
|----------------|--------------|--------------------|------------------------------|
| 全在 CXL       | 全在 CXL     | 直接读 CXL         | 纯 CXL 延迟，无 fault/迁移   |
| 当前「好划分」 | 各机本地 DRAM | 大量跨 partition 读 | fault + 迁移 + 迁移后读 CXL |

因此：

- **全在 CXL**：≈ 60s ≈ 纯计算 + 均匀的 CXL 访问延迟，**没有迁移开销**。
- **当前划分**：≈ 90s ≈ **迁移开销（约 30s）** + 计算与 CXL 访问（与 60s 同量级）。  
  即：你既付了「迁移」的代价，又没避免「最后大量数据还是在 CXL 被访问」——VMSPACE 统计里 CXL 页数很大（约 84 万页），也说明很多页被迁到 CXL 后被两机共享访问。

## 小结

- **划分「好」**只保证了：**谁负责算哪一段、以及这些段对应的数据初始落在哪台机器**。
- **没有改变**：process_edges 里 **读全图 curr、写全图 next** 的访问模式，所以必然出现「对方机器大量访问本机 DRAM」→ 大量 DSM 迁移。
- 迁移的单次成本高（跨机 + 拷贝 + TLB + 锁），次数一多就多出约 30s。
- 所以会出现：**划分看起来合理，反而比全在 CXL 慢**——因为全在 CXL 时没有迁移，只有相对均匀的 CXL 延迟。

## 可做的方向（与 process_edges_cxl_growth.md 一致）

1. **curr**：算法上需要全图读，难以从算法上避免被多机共享；要么接受迁到 CXL，要么做「按 partition 的 message passing」把贡献发到目标 partition 再合并（改算法/接口）。
2. **next**：改为「只写本 partition 的 next + 消息传递」，减少跨 partition 写，从而减少 next 触发的迁移。
3. **预放 CXL**：若迁移量巨大且难以从算法上减少，可考虑把 curr/next 等一开始就分配在 CXL，避免运行时大量迁移，用「均匀 CXL 延迟」换掉「迁移尖峰」（可能接近或略好于当前 60s 的全 CXL 行为）。
4. **批量迁移 / 预取**：在 DSM 层做批量迁移或按访问模式预取，降低单次迁移和 TLB shootdown 的摊销成本（需要内核/DSM 改动）。
