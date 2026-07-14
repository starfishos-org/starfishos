# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is **ChCore-CXL**, a research microkernel OS (based on ChCore/TreeSLS) extended with CXL memory support and Distributed Shared Memory (DSM). The system runs multiple kernel instances ("machines") in QEMU, connected via simulated CXL/ivshmem shared memory. Key research features:
- **DSM (Distributed Shared Memory)**: multiple kernel instances share a physical CXL memory region, coordinated via `kernel/dsm/`
- **Memory tiering**: DRAM-local vs CXL-backed allocation, configured in `kernel/dsm_config.cmake` (see below)
- **SSI-SLS / TreeSLS**: cross-machine and whole-system checkpoint code lives in `kernel/ckpt-ssi/` and `kernel/ckpt/`. **The current repo does NOT use these features** — they are gated off and left dormant.

## Build System

Uses CMake via the `chbuild` wrapper script, driven through the top-level `Makefile`. Build configuration is stored in `.config` (not committed). Run `make help` to list targets.

```bash
make prepare    # one-time setup after first clone: config CXL memdev + hostfs ramdisk + full build
make build      # (alias: make b) incremental build via ./chbuild build
make build-all  # (alias: make ba) clean + defconfig x86_64 + build, via scripts/quick-build.sh
make clean      # (alias: make c) reset the CXL/ivshmem memdev
```

`scripts/quick-build.sh` runs `chbuild distclean && chbuild defconfig x86_64 && chbuild build` (pass `raspi3` for the ARM target).

**启用 Demo 应用**：在 `user/demos/config.cmake` 中将对应选项设为 `ON`（默认全部 `OFF`，构建前需手动开启）：
- `CHCORE_DEMOS_GEIMINIGRAPH` — GeminiGraph（注意拼写）
- `CHCORE_DEMOS_LIGRA` — Ligra
- `CHCORE_DEMOS_REDIS` — Redis
- `CHCORE_DEMOS_MEMCACHED` / `CHCORE_DEMOS_MEMCACHETEST` — Memcached + memcachetest 负载
- `CHCORE_DEMOS_LIBEVENT` — libevent（memcached 依赖）
- `CHCORE_DEMOS_SQLITE` — SQLite3
- `CHCORE_DEMOS_LEVELDB` — LevelDB
- `CHCORE_DEMOS_ROCKSDB` — aurora-rocksdb
- `CHCORE_DEMOS_DBX1000` — DBX1000
- `CHCORE_DEMOS_PHOENIX` — Phoenix
- `CHCORE_DEMOS_GEMM` — GEMM
- `CHCORE_DEMOS_YCSB` — YCSB-C

顶层 `config.cmake` 通过 `chcore_config_include` 链式引入 `user/config.cmake` → `user/demos/config.cmake`。

### Memory tiering config (`kernel/dsm_config.cmake`)

- `DSM_SHM_DEVICE` — shared-memory backend: `IVSHMEM` / `IVSHMEM_NUMA` / `CXL_NUMA` / `CXL`.
- `DSM_MALLOC_MODE` — kernel allocation policy: `CXL` (all CXL), `DRAM` (all DRAM), `MIXED_DEFAULT_DRAM`, `MIXED_DEFAULT_CXL` (DRAM+CXL 混合，默认落 CXL).
- `DSM_USER_MALLOC_MODE` — user allocation default: `DEFAULT_DRAM` / `DEFAULT_CXL`.
- Per-object placement: `DSM_THREADCTX_MODE`, `DSM_PGTABLE_MODE`, `DSM_STACK_MODE`, `DSM_OBJECT_MODE`, `DSM_PAGE_MODE` (each `CXL` or `DRAM`).

## Running

Requires tmux and the ivshmem shared memory device. The `Makefile` wraps the common flows:

```bash
make run            # (alias: make r) single instance: reset memdev then ./build/simulate.sh
make run-2clusters  # (alias: make r2) 2-cluster run
make run-4clusters  # (alias: make r4) 4-cluster run
make r2-perf / r4-perf   # apply perf CPU config (dsm-scripts/setup/config.sh) then run
make start-ivshmem-server / kill-ivshmem-server
```

Automated benchmarks (each launches a 2-cluster run and waits for a completion marker):
`make run-mm-test`, `make run-graph-test`, `make run-dbx1000-test`, plus `.exp`-driven targets
(`make leveldb`, `make db1000`, `make gemini`, `make function-bench`, `make pca`/`kmeans`/`word_count`/… under `dsm-scripts/tests/`).

**Multi-cluster flow**: `dsm-scripts/simulate_ncluster.sh N [tag] [cmd] [marker] [--timeout=...]` orchestrates multi-instance runs — it starts ivshmem-server, launches N QEMU instances in tmux windows, waits for each to print `DSM] machine N` (DSM join), then sends a command and waits for the completion marker. `simulate_{1,2,4,6,8}clusters.sh` are thin wrappers.

### Cleanup after a test run (重要：每次跑完测试都要清理)

自动化 benchmark 模式跑完（成功、超时或中断）后**不会自动清理**——它会保留 tmux 会话和 QEMU 实例供查看。下一次测试用同一个 session 名（`$USER-qemu`），残留实例会占着共享内存、CPU 和 session 名导致冲突，所以**每次测试结束后必须清理**：

```bash
tmux kill-session -t "$USER-qemu" 2>/dev/null || true   # 杀掉 tmux 会话（连带 pane 里的 QEMU）
make kill-ivshmem-server                                # 或 ./dsm-scripts/setup/kill_ivshmem_server.sh
# 再核实没有残留 QEMU（tmux 偶尔不会回收）：
ps -eo pid,cmd | grep -E '[q]emu-6.2|[q]emu_wrapper'    # 应为空
```

清理顺序与注意事项：
1. **先 `tmux kill-session`**：QEMU 跑在 pane 里，杀会话会连带回收，这是最干净的方式。
2. 只有当上面的 `ps` 仍能查到残留 QEMU 时，才补一刀 `pkill -f qemu-6.2-system-x86_64 ; pkill -f qemu_wrapper.sh`。
   - **坑**：在 Claude 的工具调用 shell 里直接 `pkill -f qemu...` 可能误杀 shell 自身的进程组（命令以 exit code 144 / SIGTERM 退出）。优先用 `tmux kill-session`；非用 `pkill` 不可时，单独成行执行，并随后用新命令重新 `ps` 确认状态，不要被 144 误导。
3. **ivshmem-server**：`kill_ivshmem_server.sh` 按 `/tmp/ivshmem-server-$USER.pid` 杀。若马上要再跑测试，可不杀——`start_ivshmem_server.sh` 会自动杀旧的再起新的。
4. **根目录 `exec_log*.log`**：按上文 File Placement Rules 归档到 `log/<benchmark>/` 后删除，不要散落在根目录。
5. 排查残留时：`tmux ls` 看会话、`ps -eo pid,etime,cmd | grep [q]emu` 看实例（命令行里的 `machine_id=N` 标明是第几台、`name=chcore-N`）。

## Architecture

### Kernel (`kernel/`)

Capability-based microkernel. Key subsystems:

- **`kernel/dsm/`** — DSM implementation. `dsm_metadata.c` manages the shared `dsm_meta` structure (placed at start of CXL shared memory). `dsm_migrate.c` handles process migration between machines. `dsm_tiering.c` implements memory tiering (promote/demote between DRAM and CXL). `dsm_objects/` has per-object-type DSM wrappers (`cap_group.c`, `thread.c`, `vmspace.c`, `pmobject.c`, `connection.c`, `notification.c`, `irq.c`, `page.c`).
- **`kernel/mm/`** — Memory management: buddy allocator (`buddy.c`), slab (`slab.c`), `kmalloc.c`, vmregion (`vmregion.c`), page fault handler (`pgfault_handler.c`), shared memory (`shm.c`). DRAM-specific allocators in `dram_alloc.c` / `dram_slab.c`; CXL allocators in `mm/cxl/` (`cxl_alloc.c`, `cxl_slab.c`); reverse mapping for SLS in `mm/sls/rmap.c`. Page faults on CXL-backed pages trigger DSM migration (case 2.x logic).
- **`kernel/ipc/`** — IPC connections (`connection.c`), futex (`futex.c`), notifications (`notification.c`). Cross-machine IPC is an active work area (see `docs/TODO.md`, `docs/ipc.md`, `docs/fs-ipc.md`).
- **`kernel/sched/`** — Scheduler with shared queue (`dsm_meta->shared_queue`) for cross-machine work stealing.
- **`kernel/object/`** — Kernel objects: cap_group (process), thread, memory (PMO), connection, notification, etc.
- **`kernel/arch/x86_64/`** — x86_64 port; `mm/page_table.c` handles multi-level page table management including CXL-aware fill.
- **`kernel/ckpt/`, `kernel/ckpt-ssi/`** — TreeSLS / SSI-SLS checkpoint code. **Not used in this repo** (gated off).

**Compile-time gating**: most DSM code is `#ifdef DSM_ENABLED`. SSI-SLS is `#ifdef CHCORE_SSI_SLS`; TreeSLS is `#ifdef CHCORE_SLS` (`kernel/sls_config.cmake`).

**Shared memory layout** (`kernel/include/dsm/dsm-single.h`):
```
|| M0 LOCAL DRAM || M1 LOCAL DRAM || ... || CXL SHM ||
```
The `dsm_meta` struct sits at the start of CXL SHM and is accessible from all machines. It holds cluster config, memory layout, buddy/slab pools for SHM, per-CPU shared queues, and MSI message slots for inter-machine TLB shootdowns.

### User Space (`user/`)

- **`user/system-servers/`** — Microkernel system services: `procmgr` (process manager), `fsm` (FS multiplexer), `tmpfs`, `fat32`, `ext4`, `lwip` (network), `polling`, `posix_shm`, `chcore_shell`.
- **`user/libraries/`** — Ported libraries (rpmalloc, lkl-libs, etc.).
- **`user/musl-1.1.24/`** — musl libc port for ChCore.
- **`user/demos/`** — Ported applications: GeminiGraph, Ligra, redis, memcached/memcachetest, aurora-rocksdb, leveldb, sqlite, dbx1000, phoenix, YCSB-C.
- **`user/sample-apps/`** — Small test/benchmark apps.
- **`user/sys-include/`** — Shared headers between system servers.

### DSM Scripts (`dsm-scripts/`)

- `simulate_ncluster.sh` — Generic N-cluster launcher (used by `simulate_{1,2,4,6,8}clusters.sh`).
- `setup/` — environment config: `config_memdev.sh` (allocate ivshmem/CXL device from a NUMA node), `config.sh` (perf CPU config), `change_cpu_num.sh`, `prepare_cxlmem.py`, `prepare_hostfs.py` (copy files into the shared hostfs ramdisk), `start_ivshmem_server.sh` / `kill_ivshmem_server.sh`, `numa_sizes.conf`.
- `analysis/` — `parse_vmspace_stats.py`, `analyze_cxl_growth.py`.
- `bench/` — `run_gemini_sweep.sh`, `run_mm_sweep.sh`.
- `tests/` — `.exp` expect scripts for individual benchmarks (`leveldb.exp`, `db1000.exp`, `gemini.exp`, `function-bench/`, `phoenix/`, …).

## File Placement Rules

新增文件时请按以下规则放置，保持根目录整洁：

| 类型 | 放置位置 | 示例 |
|------|----------|------|
| DSM/CXL 分析脚本（Python） | `dsm-scripts/analysis/` | `parse_vmspace_stats.py`, `analyze_cxl_growth.py` |
| Benchmark 自动化脚本（Shell） | `dsm-scripts/bench/` | `run_gemini_sweep.sh`, `run_mm_sweep.sh` |
| DSM 环境配置脚本 | `dsm-scripts/setup/` | `config_memdev.sh`, `start_ivshmem_server.sh` |
| Benchmark expect 脚本 | `dsm-scripts/tests/` | `leveldb.exp`, `phoenix/pca.exp` |
| 构建/格式化等通用辅助脚本 | `scripts/` | `quick-build.sh`, `codecal.sh` |
| 项目文档、分析笔记 | `docs/` | `DEADLOCK_ANALYSIS.md`, `TODO.md` |
| Artifact evaluation 相关 | `ae/` | `artificial_eval.md` |
| 运行时/测试 log（`exec_log*.log`、benchmark 输出等） | `log/<测试名>/`（已 gitignore），按 benchmark 归类 | `log/dbx1000/`、`log/gemini/`、`log/matrix/`、`log/sweep_logs/<config>/exec_log0.log` |

**根目录只保留**：`Makefile`、`CMakeLists.txt`、`chbuild`、`config.cmake`、`Dockerfile`、`LICENSE`、`README.md`、`CLAUDE.md`、`AGENTS.md` 等顶层配置/入口文件。

**每个测试跑完后，根目录生成的 `exec_log*.log` 要归类收纳到 `log/` 下对应的测试子目录**。子目录与文件名按这次测试的**实际目的**命名，让人一看就知道在测什么（例如测 dbx1000 放 `log/dbx1000/`、测 gemini 图计算放 `log/gemini/`、矩阵乘放 `log/matrix/`；多机配置 sweep 按配置放 `log/sweep_logs/<config>/`，文件名体现 config/机器数等关键变量）。不要散落在根目录，也不要用 `exec_log0` 这种无意义的原始名。`log/` 整体已 gitignore，勿提交。

## Key Debugging

- Enable `DSM_DEBUG` in `kernel/include/dsm/dsm-single.h` for verbose DSM logs.
- Enable `MULTI_PT_DEBUG` for multi-page-table debug output.
- Enable `PGFAULT_STATS_DEBUG` in `kernel/mm/pgfault_handler.c` for page fault statistics.
- Enable `TLB_FLUSH_LATENCY_DEBUG` in `kernel/syscall/syscall.c` for TLB flush breakdown.
- Logs per QEMU instance are written to `exec_log<N>.log` in the repo root while running; after a test finishes, file them under `log/<benchmark>/` (e.g. `log/dbx1000/`, `log/sweep_logs/<config>/`).
- `dsm-scripts/analysis/parse_vmspace_stats.py` parses vmspace statistics output (enabled by the `PRINT_VMSPACE_STATS` compile flag); `analyze_cxl_growth.py` tracks CXL footprint growth.
- Resolve a faulting address to a source line: `make ip2c IP=<addr> P=<elf>` (user) or `make ip2c-kernel IP=<addr>` (kernel).
