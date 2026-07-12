# GeminiGraph Memory Structures Analysis

## Directed Graph Loading (load_directed)

### Current Understanding
For twitter-2010: vertices=41652230, edges≈1.5B (estimated from 1.7GB outgoing_adj_list)

### Phase 0-1: Out-degree Counting
- **out_degree_segments**: allocated per-machine, then merged into single `out_degree` array
  - Size: 41M vertices * 4 bytes = 164 MB
  - Allocated in local DRAM, freed after merge

### Phase 2: In-degree Initialization & outgoing_adj structure
1. **outgoing_adj_bitmap[s_i]**: Bitmap for sparse vertex representation
   - Per socket allocation
   - For 2 sockets: 2 * (41M / 8 bytes) ≈ 10 MB

2. **outgoing_adj_index[s_i]**: EdgeId array [vertices+1]
   - Per socket: 8 bytes * 41M ≈ 328 MB per socket
   - Total for 2 sockets: ~656 MB (allocated in local DRAM during load)

3. **partition_offset**: Global [partitions+1]
   - 2 * 8 bytes = 16 bytes (negligible)

4. **local_partition_offset**: [sockets+1]
   - 2 * 8 bytes = 16 bytes (negligible)

### Phase 3: Compressed Index & outgoing_adj_list
1. **compressed_outgoing_adj_index[s_i]**: CompressedAdjIndexUnit array
   - Size depends on number of vertices with outgoing edges
   - Estimate: ~5-10 MB per socket (much smaller than full index)

2. **outgoing_adj_list[s_i]**: Main adjacency list (unit_size * edges)
   - unit_size = sizeof(VertexId) = 4 bytes (for Empty EdgeData)
   - For 2 machines: 2 * (0.85B edges per partition) * 4 bytes ≈ **3.4 GB total**
   - But only **1.7 GB** shows up in CXL for partition[1]
   - This suggests partition[0] keeps its copy locally

### Runtime Structures (After Load)
1. **out_degree_local[s_i]**: Read-only copy of out_degree
   - Per socket: 41M * 4 bytes = 164 MB
   - Total: ~328 MB (DRAM copy, not shared)

2. **in_degree_local[s_i]**: Read-only copy of in_degree
   - Same size: ~328 MB (DRAM copy, not shared)

3. **curr[] / next[]**: Double arrays for pagerank values
   - Per array: 41M * 8 bytes = 328 MB each
   - ~656 MB total (detected in CXL)

4. **active->data**: VertexId bitmap/set
   - ~164 MB (already in CXL detection)

## Mystery: 926 MB Unknown CXL Growth

### Candidates
1. **send_buffer[i][s_i]->data**: Message buffers resized during process_edges
   - Resized to: sizeof(MsgUnit<double>) * owned_vertices * sockets
   - sizeof(MsgUnit<double>) = VertexId(4) + double(8) = 12 bytes
   - Per buffer: 20M * 2 * 12 = 480 MB
   - Could explain 926 MB if 2-3 buffers are allocated simultaneously

2. **recv_buffer[i][s_i]->data**: Similar to send_buffer
   - Same potential size

3. **Temporary allocations in process_edges()**:
   - emit() might accumulate messages
   - Reducer state accumulation

4. **Potential duplicate/temporary edge index arrays**:
   - If compressed_outgoing_adj_index is being expanded somewhere
   - Or temporary sorting/reordering arrays

5. **Possible mmap'd file regions**:
   - Graph file might be memory-mapped
   - Could account for large contiguous regions

## Recommended Investigation
1. Add memory allocation tracking in process_edges callback
2. Print size of send_buffer/recv_buffer after resize in process_edges
3. Search for additional malloc/mmap calls during graph initialization
4. Check if file regions are mmap'd and causing CXL growth
5. Trace which machine(s) allocate the 926MB segment
