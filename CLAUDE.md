# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Rules

- 所有测latency/crash等在代码中添加的，都需要用一个kernel的变量来控制，可以开关
- 所有

## Current Target

**研究目标**：搞清楚运行`user/demos/GeminiGraph`的pagerank程序时，**访问什么状态（哪些 VA/数据结构）导致页被迁移到 CXL**。

- **内核机制**：`kernel/mm/pgfault_handler.c` 中，当前 CPU 访问的 VA 若对应物理页在**另一台 machine 的本地 DRAM**（`get_paddr_machine_id(pa) != CUR_MACHINE_ID`），会走 DSM 的 case 2.x（migration entry / 等待迁移 / 触发迁移），该页会被迁到或复制到 CXL 共享区并映射到当前 machine，从而在 VMSPACE 统计里表现为「CXL (shared)」增加。
- **典型来源**（结合 pagerank/GeminiGraph）：图结构、文件数据、共享状态（调度队列、共享队列）。目前我遇到的问题是，当前打印的各个数据结构都不是CXL增长的主要来源。

**运行命令**：

1. 编译时需开启 **PRINT_VMSPACE_STATS**，才能在 log 中打出 VMSPACE MEMORY 统计（每段 VA 落在 CXL 还是各 Machine N）。
2. 用 `./quick-build.sh &&  ./dsm-scripts/simulate_ncluster.sh 2 pagerank_2machine.log "pagerank /host/twitter-2010.bin 41652230 50 2" "exec_time"`, 结果会输出到exec_log0.log
3. 若需应用层打印各数据结构的 VA 区间以便和 CXL 段对应，需在 pagerank/GeminiGraph 里在合适时机打印带 `[PR][part=0]` 及 `name: [0xstart-0xend)` 格式的行到同一 log。

必须要看到exec_time=xx才说明运行成功了

**Python 脚本怎么看**：

- **`parse_vmspace_stats.py`**：解析 log 中的 `[VMSPACE STATS]`，提取每段 VA 的 CXL/Machine 分布，可对比两次统计、找「新出现在 CXL」的页，并支持可视化（需 matplotlib）。用法：`python3 parse_vmspace_stats.py <log_file>`，具体选项见脚本内。
- **`analyze_cxl_growth.py`**：专门分析「第 2 次 VMSPACE STATS 比第 1 次多出来的 CXL」——从同一 log 里两段 `[VMSPACE STATS] Process: /pagerank` 中解析 CXL 段（细粒度到 `0x...-0x... -> CXL`），做差得到**新增 CXL 的 VA 区间**，并与同 log 中 `[PR][part=0]` 行的数据结构 VA（`name: [0xstart-0xend)`）做重叠匹配，输出：
  - 每个新增 CXL 段的大小、VA 区间、可能归属的数据结构；
  - 按可能数据结构汇总的新增 CXL 大小（可看出是哪些数组导致 CXL 增长）。
  - 用法：`python3 analyze_cxl_growth.py [log_file]`，默认 `exec_log0.log`；数据结构列表从同一 log 的 `[PR][part=0]` 行解析，若无则该列为「(无匹配)」。

## Overview

This is **ChCore-CXL**, a research microkernel OS (based on ChCore/TreeSLS) extended with CXL memory support and Distributed Shared Memory (DSM). The system runs multiple kernel instances ("machines") in QEMU, connected via simulated CXL/ivshmem shared memory. Key research features:
- **DSM (Distributed Shared Memory)**: multiple kernel instances share a physical CXL memory region, coordinated via `kernel/dsm/`
- **SSI-SLS (Single-System-Image Single-Level Store)**: cross-machine checkpoint/restore in `kernel/ckpt-ssi/`
- **TreeSLS**: whole-system persistent microkernel checkpoint in `kernel/ckpt/`
- **Memory tiering**: DRAM-local vs CXL-backed allocation, configured in `kernel/dsm_config.cmake`

## Build System

Uses CMake via the `chbuild` wrapper script. Build configuration is stored in `.config` (not committed).

```bash
./quick-build.sh # clean and build
./chbuild build # build without clean
```

## Running

Requires tmux and the ivshmem shared memory device.

**Multi-cluster flow**: `dsm-scripts/simulate_ncluster.sh` orchestrates multi-instance runs — it starts ivshmem-server, launches N QEMU instances in tmux windows, waits for each to print `DSM] machine N` (indicating DSM join), then sends a command to run a workload.

## Architecture

### Kernel (`kernel/`)

The kernel is a capability-based microkernel. Key subsystems:

- **`kernel/dsm/`** — DSM implementation. `dsm_metadata.c` manages the shared `dsm_meta` structure (placed at start of CXL shared memory). `dsm_migrate.c` handles process migration between machines. `dsm_tiering.c` implements memory tiering (promote/demote between DRAM and CXL). `dsm_objects/` has per-object-type DSM wrappers.
- **`kernel/ckpt/`** — current repo will not use this feature!!
- **`kernel/ckpt-ssi/`** — SSI-SLS: adds cross-machine checkpoint (`cfork.c`, `ckpt_process.c`), system-service state saving (`ckpt_sys_services.c`), and time-traveling (`ckpt_time_traveling.c`). The current repo will not use this feature!!
- **`kernel/mm/`** — Memory management: buddy allocator, slab, vmspace, page fault handler (`pgfault_handler.c`). Page faults on CXL-backed pages trigger DSM migration (case 2.x logic).
- **`kernel/ipc/`** — IPC connections and futex. Remote IPC (cross-machine) is an active work area (see `TODO.md`).
- **`kernel/sched/`** — Scheduler with shared queue (`dsm_meta->shared_queue`) for cross-machine work stealing.
- **`kernel/object/`** — Kernel objects: cap_group (process), thread, memory (PMO), connection, notification, etc.
- **`kernel/arch/x86_64/`** — x86_64 port; `mm/page_table.c` handles multi-level page table management including CXL-aware fill.

**Compile-time DSM gating**: most DSM code is `#ifdef DSM_ENABLED`. SSI-SLS code is `#ifdef CHCORE_SSI_SLS`. TreeSLS is `#ifdef CHCORE_SLS`.

**Shared memory layout** (`dsm-single.h`):
```
|| M0 LOCAL DRAM || M1 LOCAL DRAM || ... || CXL SHM ||
```
The `dsm_meta` struct sits at the start of CXL SHM and is accessible from all machines. It holds cluster config, memory layout, buddy/slab pools for SHM, per-CPU shared queues, and MSI message slots for inter-machine TLB shootdowns.

### User Space (`user/`)

- **`user/system-servers/`** — Microkernel system services: `procmgr` (process manager), `fsm` (FS multiplexer), `tmpfs`, `fat32`, `ext4`, `lwip` (network), `polling`, `posix_shm`, `chcore_shell`.
- **`user/libraries/`** — Ported libraries (rpmalloc, lkl-libs, etc.).
- **`user/musl-1.1.24/`** — musl libc port for ChCore.
- **`user/demos/`** — Ported applications: GeminiGraph, Ligra, redis, memcached, aurora-rocksdb, llama.cpp, dbx1000, cpython, etc.
- **`user/sample-apps/`** — Small test/benchmark apps.
- **`user/sys-include/`** — Shared headers between system servers.

### DSM Scripts (`dsm-scripts/`)

- `simulate_ncluster.sh` — Generic N-cluster launcher (used by `simulate_2clusters.sh`, `simulate_4clusters.sh`).
- `config_memdev.sh` — Allocates ivshmem device from a NUMA node (simulates CXL).
- `prepare_hostfs.py` — Copies files into the shared hostfs ramdisk.
- `start_ivshmem_server.sh` / `kill_ivshmem_server.sh` — Manages the ivshmem-server process.

## Key Debugging

- Enable `DSM_DEBUG` in `kernel/include/dsm/dsm-single.h` for verbose DSM logs.
- Enable `MULTI_PT_DEBUG` for multi-page-table debug output.
- Enable `PGFAULT_STATS_DEBUG` in `kernel/mm/pgfault_handler.c` for page fault statistics.
- Enable `TLB_FLUSH_LATENCY_DEBUG` in `kernel/syscall/syscall.c` for TLB flush breakdown.
- Logs per QEMU instance are written to `exec_log<N>.log`.
- `parse_vmspace_stats.py` parses vmspace statistics output (enabled by `PRINT_VMSPACE_STATS` compile flag).
