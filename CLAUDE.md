# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Language

**Use English only** in source files (`.c`, `.h`, `.py`, `.sh`, `.cmake`, etc.) and Markdown (`.md`): comments, docstrings, user-facing messages, and documentation. Do not add or restore Chinese (or other non-English natural-language) text in those files. Conversation with the user may still be in Chinese when the user prefers it; repository content must stay English.

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

**Enable demo apps**: set the corresponding options to `ON` in `user/demos/config.cmake` (all default to `OFF`; turn them on before building):
- `CHCORE_DEMOS_GEIMINIGRAPH` — GeminiGraph (note the spelling)
- `CHCORE_DEMOS_LIGRA` — Ligra
- `CHCORE_DEMOS_REDIS` — Redis
- `CHCORE_DEMOS_MEMCACHED` / `CHCORE_DEMOS_MEMCACHETEST` — Memcached + memcachetest workload
- `CHCORE_DEMOS_LIBEVENT` — libevent (memcached dependency)
- `CHCORE_DEMOS_SQLITE` — SQLite3
- `CHCORE_DEMOS_LEVELDB` — LevelDB
- `CHCORE_DEMOS_ROCKSDB` — aurora-rocksdb
- `CHCORE_DEMOS_DBX1000` — DBX1000
- `CHCORE_DEMOS_PHOENIX` — Phoenix
- `CHCORE_DEMOS_GEMM` — GEMM
- `CHCORE_DEMOS_YCSB` — YCSB-C

The top-level `config.cmake` pulls in `user/config.cmake` → `user/demos/config.cmake` via `chcore_config_include`.

### Memory tiering config (`kernel/dsm_config.cmake`)

- `DSM_SHM_DEVICE` — shared-memory backend: `IVSHMEM` / `IVSHMEM_NUMA` / `CXL_NUMA` / `CXL`.
- `DSM_MALLOC_MODE` — kernel allocation policy: `CXL` (all CXL), `DRAM` (all DRAM), `MIXED_DEFAULT_DRAM`, `MIXED_DEFAULT_CXL` (mixed DRAM+CXL, default to CXL).
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

### Cleanup after a test run (important: clean up after every test)

Automated benchmark mode does **not** clean up after finishing (success, timeout, or interrupt) — it leaves the tmux session and QEMU instances for inspection. The next test reuses the same session name (`$USER-qemu`); leftover instances hold shared memory, CPUs, and the session name and cause conflicts, so **always clean up after each test**:

```bash
tmux kill-session -t "$USER-qemu" 2>/dev/null || true   # kill tmux session (and QEMU in its panes)
make kill-ivshmem-server                                # or ./dsm-scripts/setup/kill_ivshmem_server.sh
# verify no leftover QEMU (tmux sometimes fails to reap):
ps -eo pid,cmd | grep -E '[q]emu-6.2|[q]emu_wrapper'    # should be empty
```

Cleanup order and notes:
1. **`tmux kill-session` first**: QEMU runs in panes; killing the session reaps them — this is the cleanest approach.
2. Only if `ps` still shows leftover QEMU, then `pkill -f qemu-6.2-system-x86_64 ; pkill -f qemu_wrapper.sh`.
   - **Pitfall**: running `pkill -f qemu...` directly in Claude's tool shell can kill the shell's own process group (command exits with 144 / SIGTERM). Prefer `tmux kill-session`; if you must use `pkill`, run it on its own line, then re-check with a fresh `ps` — do not be misled by exit code 144.
3. **ivshmem-server**: `kill_ivshmem_server.sh` kills via `/tmp/ivshmem-server-$USER.pid`. If you will run another test immediately, you may skip killing it — `start_ivshmem_server.sh` kills the old one and starts a new one.
4. **Root `exec_log*.log`**: archive under `log/<benchmark>/` per File Placement Rules below, then delete; do not leave them in the repo root.
5. When checking leftovers: `tmux ls` for sessions, `ps -eo pid,etime,cmd | grep [q]emu` for instances (`machine_id=N` and `name=chcore-N` in the command line identify which machine).

## Architecture

### Kernel (`kernel/`)

Capability-based microkernel. Key subsystems:

- **`kernel/dsm/`** — DSM implementation. `dsm_metadata.c` manages the shared `dsm_meta` structure (placed at start of CXL shared memory). `dsm_migrate.c` handles process migration between machines. `dsm_tiering.c` implements memory tiering (promote/demote between DRAM and CXL). `dsm_objects/` has per-object-type DSM wrappers (`cap_group.c`, `thread.c`, `vmspace.c`, `pmobject.c`, `connection.c`, `notification.c`, `irq.c`, `page.c`).
- **`kernel/mm/`** — Memory management: buddy allocator (`buddy.c`), slab (`slab.c`), `kmalloc.c`, vmregion (`vmregion.c`), page fault handler (`pgfault_handler.c`), shared memory (`shm.c`). DRAM-specific allocators in `dram_alloc.c` / `dram_slab.c`; CXL allocators in `mm/cxl/` (`cxl_alloc.c`, `cxl_slab.c`); reverse mapping for SLS in `mm/sls/rmap.c`. Page faults on CXL-backed pages trigger DSM migration (case 2.x logic).
- **`kernel/ipc/`** — IPC connections (`connection.c`), futex (`futex.c`), notifications (`notification.c`). Cross-machine IPC remains an active work area; see `docs/02-kernel-space-modules.md` and `docs/03-collaborative-system-services.md`.
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

Place new files as follows to keep the repository root clean:

| Type | Location | Examples |
|------|----------|----------|
| DSM/CXL analysis scripts (Python) | `dsm-scripts/analysis/` | `parse_vmspace_stats.py`, `analyze_cxl_growth.py` |
| Benchmark automation scripts (Shell) | `dsm-scripts/bench/` | `run_gemini_sweep.sh`, `run_mm_sweep.sh` |
| DSM environment setup scripts | `dsm-scripts/setup/` | `config_memdev.sh`, `start_ivshmem_server.sh` |
| Benchmark expect scripts | `dsm-scripts/tests/` | `leveldb.exp`, `phoenix/pca.exp` |
| Generic build/format helpers | `scripts/` | `quick-build.sh`, `codecal.sh` |
| Project docs / design guides | `docs/` | `01-design-overview.md`, `05-implementation-map.md` |
| Artifact evaluation | `artifact-evaluation/` | `README.md`, `run_all.py` |
| Runtime / test logs (`exec_log*.log`, benchmark output, etc.) | `log/<test-name>/` (gitignored), grouped by benchmark | `log/dbx1000/`, `log/gemini/`, `log/matrix/`, `log/sweep_logs/<config>/exec_log0.log` |

**Repo root should only keep**: `Makefile`, `CMakeLists.txt`, `chbuild`, `config.cmake`, `Dockerfile`, `LICENSE`, `README.md`, `CLAUDE.md`, `AGENTS.md`, and other top-level config/entry files.

**After each test, move root `exec_log*.log` into the matching subdirectory under `log/`**. Name directories and files after the **actual purpose** of the run so it is obvious what was measured (e.g. dbx1000 → `log/dbx1000/`, Gemini graph → `log/gemini/`, matrix multiply → `log/matrix/`; multi-machine config sweeps → `log/sweep_logs/<config>/`, with filenames reflecting config / machine count). Do not leave logs in the repo root, and do not keep opaque names like `exec_log0`. The whole `log/` tree is gitignored — do not commit it.

## Key Debugging

- Enable `DSM_DEBUG` in `kernel/include/dsm/dsm-single.h` for verbose DSM logs.
- Enable `MULTI_PT_DEBUG` for multi-page-table debug output.
- Enable `PGFAULT_STATS_DEBUG` in `kernel/mm/pgfault_handler.c` for page fault statistics.
- Enable `TLB_FLUSH_LATENCY_DEBUG` in `kernel/syscall/syscall.c` for TLB flush breakdown.
- Logs per QEMU instance are written to `exec_log<N>.log` in the repo root while running; after a test finishes, file them under `log/<benchmark>/` (e.g. `log/dbx1000/`, `log/sweep_logs/<config>/`).
- `dsm-scripts/analysis/parse_vmspace_stats.py` parses vmspace statistics output (enabled by the `PRINT_VMSPACE_STATS` compile flag); `analyze_cxl_growth.py` tracks CXL footprint growth.
- Resolve a faulting address to a source line: `make ip2c IP=<addr> P=<elf>` (user) or `make ip2c-kernel IP=<addr>` (kernel).
