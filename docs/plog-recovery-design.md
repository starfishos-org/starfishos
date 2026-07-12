# P-log: Ananke-style Persistent Operation Log for Filesystem Recovery

## Overview

P-log is a persistent operation log (p-log) mechanism for ChCore-CXL's tmpfs, inspired by Ananke. It enables transparent filesystem server recovery after machine crashes in a distributed shared memory (DSM) microkernel environment.

**Problem**: In ChCore-CXL, each machine runs an independent tmpfs instance as a user-space server. When a machine crashes, its tmpfs state (all files, directories, in-flight operations) is lost. Clients on surviving machines that were accessing the crashed tmpfs via cross-machine IPC (durable queue on CXL) lose connectivity.

**Solution**: Each tmpfs instance maintains an append-only operation log on CXL shared memory. Since CXL memory survives individual machine crashes, a recovery tmpfs instance on a surviving machine can map the crashed machine's p-log, replay the logged operations to reconstruct the filesystem state, and resume serving client requests from the same durable queue.

## Architecture

```
Machine 0 (original)          CXL Shared Memory              Machine 1 (client + recovery)
+-------------------+    +---------------------------+    +----------------------------+
|  tmpfs instance   |--->| P-log (Machine 0)         |<---| recovery tmpfs instance    |
|  - creat -> log   |    | [HDR][CREAT /f][WRITE /f] |    | - map remote p-log         |
|  - write -> log   |    +---------------------------+    | - replay: creat, write     |
|                   |    | Durable Queue (SHM ID 0)  |<---| - start polling on queue   |
+-------------------+    | [node][node][node]...     |    +----------------------------+
        |                +---------------------------+           |
        v (crash)              ^                                 v
        X                      |                          Client continues
                          Client on Machine 1
                          enqueues via polling
```

## Components

### 1. CXL SHM Allocation (Kernel)

Each machine gets a dedicated 1 MB p-log region on CXL shared memory, allocated during kernel SHM initialization alongside the existing durable queue regions.

- **SHM ID mapping**: Machine N's p-log uses SHM ID `CLUSTER_MAX_MACHINE_NUM + N` (e.g., machine 0's p-log = SHM ID 8, machine 1's = SHM ID 9).
- **`MAX_SHM_NUM`** doubled to `2 * CLUSTER_MAX_MACHINE_NUM` to accommodate both durable queues and p-logs.
- **`sys_mmap_shm()`** handles different region sizes: durable queue = 256 KB, p-log = 1 MB.

**Files**: `kernel/include/dsm/dsm-single.h`, `kernel/include/mm/shm.h`, `kernel/mm/shm.c`

### 2. P-log Data Structure (User-space)

The p-log is an append-only log with a fixed header followed by variable-size entries.

**Header** (`struct plog_header`, 64 bytes, cache-line aligned):
| Field | Type | Description |
|-------|------|-------------|
| `magic` | `uint32_t` | `0x504C4F47` ("PLOG") for validation |
| `state` | `volatile uint32_t` | `INACTIVE` / `ACTIVE` / `CRASHED` |
| `tail` | `volatile uint64_t` | Byte offset of next write position |
| `capacity` | `uint64_t` | Usable bytes (region size - header) |
| `seq_counter` | `uint64_t` | Monotonic sequence number |
| `owner_machine` | `uint32_t` | Machine ID that owns this p-log |

**Entry** (`struct plog_entry`, variable size, packed):
| Field | Type | Description |
|-------|------|-------------|
| `op` | `uint32_t` | `PLOG_OP_CREAT` (1) or `PLOG_OP_WRITE` (2) |
| `entry_len` | `uint32_t` | Total bytes of this entry |
| `seq` | `uint64_t` | Sequence number |
| `path` | `char[256]` | File path |
| (union) | | `creat.mode` or `write.{offset, data_len, data[]}` |

**Crash consistency protocol**:
1. Write entry body at `tail` offset
2. `clwb` + `sfence` (flush entry to CXL)
3. Update `tail` to `tail + entry_len`
4. `clwb` + `sfence` (flush tail pointer)

On recovery, entries beyond `tail` are ignored, guaranteeing that only fully-written entries are replayed.

**Files**: `user/system-servers/tmpfs/plog.h`, `user/system-servers/tmpfs/plog.c`

### 3. Operation Logging (tmpfs Instrumentation)

Two operations are logged (MVP scope):

- **`CREAT`**: Logged in `tmpfs_open()` when `O_CREAT` flag is set and the open succeeds. Records: path, mode.
- **`WRITE`**: Logged in `tmpfs_write()` after a successful write. Records: path, offset, inline data.

**Inode-to-path tracking**: Since `tmpfs_write()` receives only an inode pointer (not a path), we maintain a lightweight inode-to-path map. When a file is opened, `plog_track_inode(inode, path)` records the mapping. On write, `plog_get_inode_path(inode)` retrieves the path for logging.

**Files**: `user/system-servers/tmpfs/tmpfs_ops.c`, `user/system-servers/tmpfs/tmpfs.c`

### 4. Recovery Path

When a machine crashes, a surviving machine can start a recovery tmpfs instance:

```
tmpfs.srv --recover <crashed_machine_id>
```

Recovery steps:
1. **Init empty tmpfs** — create root inode and directory structures
2. **Map remote p-log** — `usys_mmap_shm(PLOG_SHM_ID(crashed_mid))` maps the crashed machine's p-log from CXL
3. **Replay** — iterate entries from header to `tail`:
   - `PLOG_OP_CREAT`: call `__fs_creat(path, mode)` to create file
   - `PLOG_OP_WRITE`: look up inode by path, call `tfs_file_write(inode, offset, data, len)`
4. **Init own p-log** — start fresh p-log for the recovery instance
5. **Register IPC server** — become available for new IPC connections

**Files**: `user/system-servers/tmpfs/tmpfs.c` (`recover_tmpfs()`)

### 5. Durable Queue Recovery

The cross-machine IPC durable queue lives on CXL (SHM ID = target machine ID). After the consumer machine crashes:

- `dq_recover_crash(shm)`: walks the queue, marks any `DOING` nodes as `CRASH`
- The recovery tmpfs can start a new polling thread on the same queue
- Clients detect `CRASH` status in `dq_wait_for_done()` and can retry

**Files**: `user/system-servers/polling/polling_server.c`, `user/system-servers/polling/polling_req.c`

### 6. FSM Integration

A new FSM request type `FSM_REQ_REPLACE_FS` allows a recovery instance to replace the mount point's filesystem capability:

```c
case FSM_REQ_REPLACE_FS:
    mp->fs_cap = new_cap;
    mp->_fs_ipc_struct = ipc_register_client(new_cap);
    mp->target_machine_id = current_machine_id;
```

**Files**: `user/sys-include/chcore-internal/fs_defs.h`, `user/system-servers/fsm/fsm.c`

## Test

`dsm-scripts/test_plog_recovery.sh` orchestrates a 4-phase test:

1. **Write**: Machine 1 client writes known data to machine 0's tmpfs via cross-machine polling
2. **Crash**: Machine 0 is killed (QEMU terminated)
3. **Recover**: Machine 1 runs `tmpfs.srv --recover 0`, replaying p-log from CXL
4. **Verify**: Machine 1 client reads back data from recovered tmpfs, verifies match

Test client: `plog_test_client.bin -s <shm_id> -m write|verify`

## File Summary

| File | Change |
|------|--------|
| `kernel/include/dsm/dsm-single.h` | `MAX_SHM_NUM = 2 * CLUSTER_MAX_MACHINE_NUM` |
| `kernel/include/mm/shm.h` | `PLOG_SHM_SIZE`, `PLOG_SHM_ID_BASE`, `PLOG_SHM_ID(mid)` |
| `kernel/mm/shm.c` | Init p-log SHM regions; size-aware `sys_mmap_shm` |
| **NEW** `user/system-servers/tmpfs/plog.h` | P-log structures, API declarations |
| **NEW** `user/system-servers/tmpfs/plog.c` | P-log init, append, replay, inode-path tracking |
| `user/system-servers/tmpfs/tmpfs.c` | `plog_init()` at startup; `recover_tmpfs()` entry point |
| `user/system-servers/tmpfs/tmpfs_ops.c` | P-log append calls in `tmpfs_open` and `tmpfs_write` |
| `user/system-servers/tmpfs/CMakeLists.txt` | Add `plog.c` |
| `user/sys-include/chcore-internal/fs_defs.h` | `FSM_REQ_REPLACE_FS` |
| `user/system-servers/fsm/fsm.c` | Handle `FSM_REQ_REPLACE_FS` |
| `user/system-servers/polling/polling_server.c` | `dq_recover_crash()` |
| `user/system-servers/polling/polling_req.c` | CRASH detection in `dq_wait_for_done` |
| **NEW** `user/system-servers/polling/plog_test_client.c` | Test client |
| **NEW** `dsm-scripts/test_plog_recovery.sh` | Test orchestration |
