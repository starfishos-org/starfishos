# compute 期间访问 outgoing_adj_list 的位置总结

## 1. 结论摘要

- **dense 模式下**：compute 期间**没有任何代码路径**通过 `outgoing_adj_list[s_i]` 这个**名字**去访问；同一块内存在 dense 时是以 `incoming_adj_list[s_i]` 被访问的（见下）。
- **outgoing_adj_list[1] 的 1.70 GB** 来自 **load 时按边数分配**：`outgoing_adj_list[s_i] = malloc(unit_size * outgoing_edges[s_i])`。`outgoing_edges[1]` = 目标点在 partition 1 的边数，由图和划分决定，和是否 dense 无关。
- 在 directed 图且当前实现下，**incoming_adj_list 和 outgoing_adj_list 指向同一块内存**（见 graph.hpp 1306–1309）。因此：
  - 这块 1.70 GB 在 dense 时**仍然被使用**，只是以 `incoming_adj_list[1]` 的名义、且仅由 **socket 1（machine 1）** 的线程在 dense_signal 里访问。
  - 若统计或报表里把「outgoing_adj_list[1]」和「incoming_adj_list[1]」分开算，会看到同一块内存在两处都出现；若只关心「谁在 compute 时访问了这块内存」，则只有 dense_signal 里对 `incoming_adj_list[s_i]` 的访问，且已限制在同 socket，不会跨机 steal 访问。

下面按「仅读 outgoing_adj_list」和「读同一块内存（incoming = outgoing）」分别列出 compute 相关位置。

---

## 2. 仅通过 `outgoing_adj_list` 访问的位置（且仅在 sparse 分支）

在 `graph.hpp` 的 `process_edges` 里，**只有 sparse 分支**会直接用 `outgoing_adj_list[s_i]`：

| 行号 | 位置 | 说明 |
|------|------|------|
| 2379 | sparse 分支内层循环 | `sparse_slot(v_i, msg_data, VertexAdjList<EdgeData>(outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i], outgoing_adj_list[s_i] + outgoing_adj_index[s_i][v_i+1]))` |
| 2402 | sparse 分支 steal 循环 | 同上，偷到任务后用 `outgoing_adj_list[s_i]` 构造 `VertexAdjList` 调 `sparse_slot` |

当前代码里 **sparse 被写死为 0**（约 2280 行：`bool sparse = 0;`），因此这两处在实际 run 时**不会执行**。  
也就是说：**在现有配置下，compute 期间没有任何地方以 `outgoing_adj_list` 的名义访问该数组。**

---

## 3. 通过 `incoming_adj_list` 访问（与 outgoing 同一块内存）

Directed 图 load 结束后有（graph.hpp 1306–1309）：

```cpp
incoming_adj_list = outgoing_adj_list;
```

因此对 `incoming_adj_list[s_i]` 的访问就是在访问与 `outgoing_adj_list[s_i]` **同一块**的 1.70 GB。

在 **dense 分支**里，只有下面两处会在 compute 时访问这块内存：

| 行号 | 位置 | 说明 |
|------|------|------|
| 2482 | dense_signal 主循环 | `dense_signal(v_i, VertexAdjList<EdgeData>(incoming_adj_list[s_i] + compressed_incoming_adj_index[s_i][p_v_i].index, ...))`，其中 `s_i = get_socket_id(thread_id)` |
| 2510 | dense_signal steal 循环 | 同上，偷到任务后用 `incoming_adj_list[s_i]` 调 `dense_signal` |

并且 dense 的 work-stealing 已限制为**同 socket**（2492–2494 行）：

```cpp
if (get_socket_id(t_i) != get_socket_id(thread_id))
  continue;
```

因此：
- 只有 `get_socket_id(thread_id) == 1` 的线程会访问 `incoming_adj_list[1]`（即同一块 1.70 GB）；
- 这些线程只会是 machine 1 上的线程，不会出现 machine 0 去访问 `outgoing_adj_list[1]` / `incoming_adj_list[1]` 的情况。

---

## 4. 其他可能相关但未访问 outgoing_adj_list 的 compute 路径

- **process_edges 开头**（2273–2278）：用 `process_vertices` 算 `active_edges`，内部只用 `get_out_degree_for_thread(vtx)`，读的是 `out_degree_local[s_i][vtx]` 或 `out_degree[vtx]`，**不读** `outgoing_adj_list`。
- **dense_slot 阶段**（约 2556–2565）：只做 `dense_slot(v_i, msg_data)`，不传邻接表，**不访问** `outgoing_adj_list` / `incoming_adj_list`。
- **process_vertices**：只按 `active` 和用户传入的 `process`，没有邻接表参数，**不访问** `outgoing_adj_list`。
- **toolkits（如 pagerank.cpp）**：只有对 `graph->outgoing_adj_list[s_i]` 的 **printf 地址**，没有对内容的遍历，不算 compute 访问。

---

## 5. 为什么 outgoing_adj_list[1] 仍是 1.70 GB？

- **容量**：  
  `outgoing_adj_list[s_i]` 在 load 时分配（例如 load_directed 里约 1250–1251 行）：  
  `outgoing_adj_list[s_i] = (AdjUnit<EdgeData> *)malloc(unit_size * outgoing_edges[s_i]);`  
  `outgoing_edges[1]` = 目标顶点在 partition 1 的边数，由图和划分方式决定。1.70 GB 即 `unit_size * outgoing_edges[1]`，与是否用 dense 无关；dense 只改变**遍历方式**，不改变**边数**。

- **是否被使用**：  
  在 directed 图下这块内存在 compute 时以 `incoming_adj_list[1]` 被使用（dense_signal）；和 outgoing 是同一块，所以不能「因为用 dense 就不分配 outgoing_adj_list[1]」，否则 incoming_adj_list[1] 也没了。

- **若仍看到 CXL 增长**：  
  若在「全部 dense、且 dense 不跨 socket steal」的前提下，仍看到与 `outgoing_adj_list[1]` 相关的 CXL 增长，可能原因包括：  
  1) load 阶段该 buffer 的首次接触不在 M1（例如分配/Phase3 写不在 M1）；  
  2) 统计方式把同一块内存既算成 outgoing 又算成 incoming，导致重复或混淆；  
  3) 其他非 process_edges 的路径（例如初始化、调试打印、或其它算法阶段）首次接触该区间。  
  需要根据具体统计/报表是在哪一阶段、按什么 key（地址区间 vs 名字）来区分。

---

## 6. 汇总表：compute 期间谁访问了「outgoing_adj_list[1] 这块内存」

| 访问方式 | 代码位置 | 是否执行（当前 sparse=0） | 访问者线程 |
|----------|-----------|---------------------------|------------|
| 以 `outgoing_adj_list[1]` 访问 | 2379, 2402 | 否（sparse 分支未走） | - |
| 以 `incoming_adj_list[1]` 访问（同一块） | 2482, 2510 | 是（dense_signal） | 仅 `get_socket_id(thread_id)==1`，即 machine 1 |

因此：**compute 期间会碰到「outgoing_adj_list[1] 这块 1.70 GB」的，只有 dense_signal 里对 incoming_adj_list[1] 的访问，且仅限 machine 1 的线程；dense 下已不做跨机 steal，不会出现 machine 0 访问该块。**  
若仍希望进一步减小 1.70 GB 的「影响」（例如 CXL 或首次接触），需要从 **load 时分配/首次接触落在哪台机器** 以及 **统计口径** 上排查，而不是从「compute 里还有谁读 outgoing_adj_list」入手——compute 里已经只有 incoming、且已按 socket 限制。
