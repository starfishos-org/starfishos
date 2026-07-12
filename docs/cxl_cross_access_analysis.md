# 为何这些数据结构会被两台机器一起访问

## 1. 根因一：`tune_chunks()` 在单线程上遍历所有 socket（主要来源）

**位置**：`graph.hpp` 中 `tune_chunks()`（约 2041–2125 行），在 **load_directed() 末尾** 被调用。

**执行上下文**：  
`load_directed()` 由 **main 线程** 调用（pagerank 里 `graph->load_directed(argv[1], ...)` 在 main 中、且前面有 `usys_set_affinity(-2, 0)`），因此通常跑在 **machine 0** 上。

**行为**：
- `tune_chunks()` 里对每个 partition 做：
  - `for (int step=0; step<partitions; step++)`
  - 内层 `for (int t_i=0; t_i<threads; t_i++)`，并用 `s_i = get_socket_id(t_i)`。
- 因此会访问：
  - `compressed_incoming_adj_vertices[s_i]`
  - `compressed_incoming_adj_index[s_i][p_v_i]`
  当 `t_i` 遍历 0..threads-1 时，`s_i` 会取遍 0 和 1（两台机器）。
- 即：**同一个线程（main，在 M0 上）会读 compressed_incoming_adj_index[0] 和 [1]、compressed_incoming_adj_vertices[0] 和 [1]**。  
  于是 **machine 0 上的线程在 load 结束时就会触碰 machine 1 上的 compressed_incoming_adj_* 等数据**，这些页会被映射/迁移到 CXL 或经互连被 M0 访问，形成“两台机器一起访问”的观感。

**结论**：  
**outgoing_adj_list / compressed_outgoing_adj_index 等被“两台机器一起访问”的一个主要来源是：tune_chunks() 在单线程（main）上按所有 socket 遍历，读了所有 socket 的 compressed_incoming_adj_*，导致 M0 访问了 M1 的数据。**

---

## 2. 根因二：process_edges dense 路径的 work stealing（已修复）

**位置**：`process_edges` 里 dense_signal 的 stealing 循环（约 2493–2510 行）。

**原问题**：  
Stealing 时没有限制“只偷同 socket”，于是 machine 0 上的线程可能从 machine 1 的线程偷任务，并用 `s_i = get_socket_id(t_i)`（被偷线程的 socket）去读 `incoming_adj_list[s_i]`、`compressed_incoming_adj_index[s_i]`，即读 **incoming_adj_list[1]** 等，造成 M0 访问 M1 的 adj 数据。

**当前状态**：  
已在该 stealing 循环中加上 `if (get_socket_id(t_i) != get_socket_id(thread_id)) continue;`，只允许同 socket 内偷任务，该路径的跨机访问已消除。

---

## 3. 根因三：DSM 共享地址空间与迁移策略

- 两机同属一个进程，**共享同一套 VA**，`outgoing_adj_list[0]`、`outgoing_adj_list[1]` 等在所有线程里都是合法指针。
- 内核/DSM 的 **tiering / 迁移策略** 可能把“冷”页或大块堆统一迁到 CXL，因此即使没有应用层跨机读，两边的数据也可能都出现在 CXL 里，表现为“这些数据结构所在 VA 段被两台机器（或同一进程的不同线程）一起使用”。

---

## 4. 小结与可做改动

| 原因 | 是否仍存在 | 说明 |
|------|------------|------|
| **tune_chunks() 单线程遍历所有 socket** | 是 | main 在 M0 上跑 load_directed，结束时 tune_chunks() 读遍 compressed_incoming_adj_index[0/1] 等，M0 会碰到 M1 的数据。 |
| **dense_signal 跨 socket stealing** | 否（已修） | 已限制为同 socket 内 stealing，不再为跨机访问来源。 |
| **DSM 共享 VA + 迁移** | 是 | 与实现策略相关，非单一代码路径。 |

若希望进一步减少“两台机器一起访问”这些结构，可考虑：

- **tune_chunks()**：改为按 partition/socket 拆分，让每个 partition 的 chunk 计算只在对应 machine 上做（例如通过 InvokePerMachine 或按 socket 的并行），避免单线程在 M0 上读 compressed_incoming_adj_index[1] 等；  
  或至少保证只读本机的 compressed_incoming_adj_*（例如只遍历本 socket 的 thread），不读对端 socket 的索引/邻接表。

以上即这些数据结构会被两台机器一起访问的主要原因分析。
