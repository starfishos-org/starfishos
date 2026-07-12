# CXL 迁移分析：从 exec_log0.log 看“访问什么导致页迁到 CXL”

## 1. Log 与统计含义

- **exec_log0.log** 来自 **vm_id: 0**（单台 QEMU，单内核实例）。pagerank 进程在该内核上跑，配置了 2 个逻辑 machine（`SetThreadCount: 16 threads, 2 machines, 8 threads/machine`），即两个 CPU 组（machine 0 / machine 1）共享同一 vmspace。
- **VMSPACE MEMORY 统计**（`kernel/mm/vmregion.c` 里 `print_vmspace_memory_summary`）：
  - 对 vmspace 里每个 VA，遍历**每个 machine 的页表**，若该 VA 已映射则取物理页地址 `pa`，用 `get_paddr_machine_id(pa)` 分类：
    - `mid == MACHINE_ID_SHARED_MEMORY` → 计入 **CXL (shared)**；
    - `mid == 0,1,...` → 计入 **Machine N**（该页在机器 N 的本地 DRAM）。
  - 因此：**“CXL (shared)”增加 = 有更多页的物理地址落在 CXL 共享区**（被迁到或分配到 CXL）。

## 2. Log 里 CXL 何时增长

从 log 前几百行可以清楚看到 CXL 在**哪类用户活动后**增加：

| 行号区间 | CXL (shared) 变化 | 紧挨着的用户态打印 / 阶段 |
|----------|-------------------|----------------------------|
| 95–100   | 324494（初始）    | 打开 hostfs 图文件后，第一次打 stats |
| 103–110  | **324494 → 324601** (+107) | `func thread_id = 0, 7`；`[0] begin_p_v_i = 0, final_p_v_i = 0`；thread 0/7 进入 dense 步、部分区间为空 |
| 133–139  | **324601 → 324896** (+295) | `func thread_id = 1, 5, 3`；`[1] begin_p_v_i = 64` 等，thread 1/3/5 开始跑 partition 1 的 dense |
| 138–143  | **324896 → 324918 → 324946** | `[1] thread_state[thread_id]->status = STEALING`；thread 2/6 参与，进入 work stealing |
| 151–162  | **324946 → 324956** (+10)   | `func thread_id = 4, 6`；`[3][5][6] begin_p_v_i...`；更多线程进入 dense / stealing |
| 之后     | 324956 稳定一段时间        | `[8] begin_p_v_i = 21560337`，`[8] for loop v_i = 22462528` 等，大量对顶点的循环 |

结论：**CXL 的上升与 “dense 阶段 + work stealing” 强相关**——每当有新的 thread 开始跑 dense_signal（或进入 STEALING 偷别机/别线程的区间），stats 里 CXL 就多一批页。

## 3. 是谁的访问导致“别人的内存”被迁到 CXL

- 内核里“页迁到 CXL”的路径在 **page fault 处理**（`kernel/mm/pgfault_handler.c`）：当前 CPU 访问的 VA 若对应物理页在**另一台 machine 的本地 DRAM**（`get_paddr_machine_id(pa) != CUR_MACHINE_ID`），会走 DSM 的 case 2.x（migration entry / 等待迁移 / 触发迁移），最终该页会被迁到或复制到 CXL 共享区并在当前 machine 的页表里映射，从而被算进 “CXL (shared)”。
- 因此：**“访问别人的内存” = 当前线程访问的 VA 所对应的物理页，当前正落在另一 machine 的本地 DRAM 上**。一次这样的访问触发一次 fault → 一次迁移/复制到 CXL → CXL 计数增加（或“别人机器”的计数减少）。

结合 log 里出现的位置，这些访问都发生在 **process_edges(dense)** 的这段逻辑里：

1. **`compressed_incoming_adj_index[s_i][p_v_i]`**  
   - 按 socket/partition 的压缩入边索引；线程按 `begin_p_v_i`～`end_p_v_i` 遍历，不同 thread 可能偷到“属于另一 machine 的”区间 → 访问到在另一 machine DRAM 上的页。

2. **`incoming_adj_list[s_i] + index`**  
   - 入边邻接表；`dense_signal(v_i, VertexAdjList(...))` 会根据索引访问这段内存。顶点 `v_i` 若属于另一 partition（另一 machine 负责），对应页往往在另一 machine 的 DRAM → 被当前 machine 访问时迁到 CXL。

3. **`dense_signal` 内部访问的顶点/边数据**  
   - 例如 pagerank 的 rank 数组、消息 buffer 等；若这些数组按顶点区间分布在不同 machine 的本地分配，则“本机线程偷到别机顶点”时就会访问到这些页 → 同样触发迁移到 CXL。

4. **`thread_state[t_i]`**  
   - 多线程共享的调度状态；若 thread_state 所在页被分配在某一 machine 的 DRAM，另一 machine 的线程在 stealing 时读/写 `thread_state[t_i]->curr/end/status` 也会触发 fault，可能把该页迁到 CXL。

## 4. 总结：什么访问导致迁到 CXL

- **直接原因**：当前 CPU（某 machine）访问的 **VA 对应的物理页当时在另一 machine 的本地 DRAM**，触发 page fault → DSM 将该页迁到或复制到 CXL 并映射到当前 machine。
- **在 log 对应的代码路径上**，这些 VA 主要来自：
  - **图结构**：`compressed_incoming_adj_index`、`incoming_adj_list`（按 partition/socket 划分，被多机多线程共享访问）；
  - **dense_signal 内**：顶点/边的应用数据（如 rank、消息等），按顶点区间分布在多机上；
  - **调度/共享状态**：`thread_state` 等被多线程跨 machine 访问的结构。
- **为何 CXL 在 log 里“一步步涨”**：每个 step 中，随着更多 thread 参与（`func thread_id = 0,1,3,4,5,6,7,8`）以及 work stealing（`status = STEALING`、`begin_p_v_i` 跨区间），**跨 partition / 跨 machine 的访问增多**，更多“别人 DRAM 上的页”被本机访问 → 更多页迁到 CXL，与 log 中 CXL 从 324494 涨到 324956 的节奏一致。

若要精确定位到“某次增长是哪个 VA / 哪个 vmr”，需要在内核的 pgfault 路径（case 2.x 分支）里对 fault_addr 和迁移结果打日志，再结合 `parse_vmspace_stats` 或 vmr 的 VA 区间反推是哪个数组或哪段图数据。
