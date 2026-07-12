# outgoing_adj_list[1] 迁移到 CXL 的原因分析

## 1. 现象

`analyze_cxl_growth.py` 显示 compute 阶段后 `outgoing_adj_list[1]` 全部（约 1.7GB）迁移到 CXL，说明 **machine 0 错误地访问了 machine 1 的 outgoing_adj_list[1]**。

## 2. 数据结构语义

- `outgoing_adj_list[s_i]`：按 dst 的 local_partition 分区，存边 (src, dst)
- `outgoing_adj_bitmap[s_i]->get_bit(src)`：为真表示 src 有出边在 `outgoing_adj_list[s_i]` 中
- 在 ChCore 中：`outgoing_adj_list[s_i]` 由 loader s_i 在 InvokePerMachine 内分配 → 落在 machine s_i 上

## 3. sparse mode 的 process_edges 流程（原逻辑）

```cpp
used_buffer = send_buffer[partition_id];
for (int s_i=0; s_i<sockets; s_i++) {
  buffer = used_buffer[s_i];  // send_buffer[partition_id][s_i]
  // 按 b_i 把 buffer 分给所有 threads（跨机）
  Parallel::Reduce(func, threads);
}
```

- 每个 thread 用 `s_i = get_socket_id(thread_id)` 访问 `outgoing_adj_list[s_i]`
- 工作按 **buffer 下标 b_i** 划分，而非按「该 (src, msg) 需要哪个 outgoing_adj_list」

## 4. 根因

当某个 (src, msg) 需要 `outgoing_adj_list[1]`（即 `get_bit(1)` 为真）时，它可能被分到 machine 0 的 thread（基于 b_i）。此时该 thread 的 `get_socket_id=0`，只会检查 `get_bit(0)` 并跳过，导致：
- 若存在某种路径下 machine 0 访问了 [1]，则会导致 CXL 迁移
- 更可能：当 sockets>1 且 bind_cpu/loader 配置使部分「socket 1」线程落在 machine 0 上时，machine 0 会访问 `outgoing_adj_list[1]`

## 5. Dense 路径的同类问题

Dense 模式下使用 `incoming_adj_list[s_i]` 和 `compressed_incoming_adj_index[s_i]`（与 outgoing 同源）。**work-stealing 未限制同 socket** 时，machine 0 的线程可能从 machine 1 的线程偷任务，并用 `s_i = get_socket_id(t_i) = 1` 访问 `incoming_adj_list[1]`，导致 machine 0 跨机访问、CXL 迁移。

**修复**：在 dense 的 stealing 循环中增加与 process_vertices 一致的限制：`if (get_socket_id(t_i) != get_socket_id(thread_id)) continue;`，只允许同 socket 内窃取。

## 6. 修复：使用 InvokeOnMachine 保证数据局部性（sparse）

改为对每个 s_i **仅在 machine s_i 上**处理需要 `outgoing_adj_list[s_i]` 的 (src, msg)：

- 对每个 s_i 调用 `Parallel::InvokeOnMachine(s_i, ...)`，只把相关任务投递到 machine s_i
- 遍历所有 buffer，对每个 (src, msg) 若 `outgoing_adj_bitmap[s_i]->get_bit(v_i)` 则在本机执行 sparse_slot
- 从而保证 `outgoing_adj_list[s_i]` 只被 machine s_i 访问，避免 CXL 迁移
