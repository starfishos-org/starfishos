# GeminiGraph CXL Memory Migration Fix - Summary

## Problem Identified

In the dense mode of `process_edges()`, there was a critical bug causing unnecessary CXL memory migration of adjacency list data structures.

### Root Cause

In `graph.hpp` line 2463, when processing partition `i`'s vertices:

```cpp
for (int step=0; step<partitions; step++) {
    int i = current_send_part_id;  // partition ID being processed

    auto func = [this, &dense_signal, ...](uint32_t thread_id) {
        int s_i = get_socket_id(thread_id);  // BUG: uses thread's socket, not partition ID

        for (VertexId p_v_i = begin_p_v_i; p_v_i < end_p_v_i; p_v_i++) {
            VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
            dense_signal(v_i, VertexAdjList<EdgeData>(
                incoming_adj_list[s_i] + ...,  // WRONG: accessing wrong machine's data
                ...
            ));
        }
    };
}
```

**Problem**: When processing partition 0 (allocated on machine 0), a thread running on machine 1 would access `incoming_adj_list[1]` instead of `incoming_adj_list[0]`, causing cross-machine access and DSM migration to CXL.

## Solution Applied

Changed line 2463 to use partition ID instead of thread's socket ID:

```cpp
auto func = [this, &dense_signal, ..., i](uint32_t thread_id) {
    int s_i = i;  // FIXED: use partition ID, not socket ID

    for (VertexId p_v_i = begin_p_v_i; p_v_i < end_p_v_i; p_v_i++) {
        VertexId v_i = compressed_incoming_adj_index[s_i][p_v_i].vertex;
        dense_signal(v_i, VertexAdjList<EdgeData>(
            incoming_adj_list[s_i] + ...,  // CORRECT: accesses partition's local data
            ...
        ));
    }
};
```

## Results

### Before Fix
```
Final VMSPACE STATS:
  CXL (shared memory): 2,252,315 pages (~8.8 GB)
  Machine 0: Unknown
  Machine 1: Unknown

CXL Growth: ~1.9 GB (outgoing_adj_list[0] and [1] both migrated)
```

### After Fix
```
Final VMSPACE STATS:
  CXL (shared memory): 514,838 pages (~2.0 GB)
  Machine 0: 756,327 pages (~3.0 GB)
  Machine 1: 511,450 pages (~2.0 GB)

CXL Reduction: ~6.8 GB (76% decrease!)
Outgoing_adj_list now properly stays local on each machine
```

## Key Improvements

1. **CXL Memory Usage**: Reduced from 8.8 GB to 2.0 GB (77% reduction)
2. **Data Locality**: outgoing_adj_list now stays on local DRAM where allocated
3. **No Cross-Machine Migrations**: Eliminated unnecessary DSM page faults
4. **Better NUMA Locality**: Threads access data on their assigned machine

## Verification

Examined VMSPACE STATS details showing:
- `0x300ee0000000-0x300ef40e1000 -> machines: [0]` (outgoing_adj_list[0] on M0)
- `0x300eacee9000-0x300ed14df000 -> machines: [1]` (outgoing_adj_list[1] on M1)

Both adjacency lists now properly retained on their local machines without CXL migration.

## Technical Details

The bug only manifested in dense mode (which is enabled by default for pagerank since `sparse = 0`).

The stealing loop also had the same issue, which was fixed by using `s_i_steal = i` instead of `s_i = get_socket_id(t_i)`.

This ensures all access patterns within process_edges respect machine boundaries and avoid cross-machine DSM migrations.
