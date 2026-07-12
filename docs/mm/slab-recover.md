# CXL Slab Crash Recovery

## 1. Problem

CXL slab allocator metadata resides in persistent shared memory. If a machine crashes mid-alloc or mid-free, `slab_header` fields (`free_list_head`, `current_free_cnt`) can be left in an inconsistent state. On recovery, the slab is corrupt.

## 2. Design: Per-CPU In-Flight Undo Log

### 2.1 Core Idea

Each CPU has exactly one slab operation in-flight at a time (protected by a spinlock). We place a **per-CPU undo log** in CXL shared memory (`dsm_meta`). Before mutating a slab, the CPU snapshots the old values into its log and sets an `op` flag. After persisting the mutation, it clears `op`. On recovery, any CPU log with `op != NONE` triggers an undo on the referenced slab.

Key design choices:
- **Log in CXL, not in slab_header**: `slab_header` stays unmodified (no size increase). Recovery checks N per-CPU logs instead of scanning all CXL pages.
- **Pool metadata in CXL**: `cxl_slab_pool` moves from DRAM to `dsm_meta`, surviving crashes. Recovery walks pool lists directly.
- **Full slab tracking**: a `full_slab_list` is added to `slab_pointer` so recovery can find full slabs (which are not on `current_slab` or `partial_slab_list`).

### 2.2 Compile-Time Gate

All recovery code is guarded by `#ifdef SLAB_CRASH_RECOVERY`. When disabled (default), log helpers compile to empty macros -- zero overhead.

```cmake
# kernel/dsm_config.cmake
set(SLAB_CRASH_RECOVERY "OFF")   # "ON" to enable
```

### 2.3 Data Structures

**Per-CPU log** (in `dsm_meta`, survives crash):
```c
struct slab_cpu_log {
    volatile u8   op;           // SLAB_OP_NONE / ALLOC / FREE
    u8   _pad[5];
    u16  old_free_cnt;          // current_free_cnt before mutation
    void *slab_addr;            // which slab is being operated on
    void *old_free_head;        // free_list_head before mutation
};
```

**Extended slab_pointer** (full_slab_list):
```c
struct slab_pointer {
    struct slab_header *current_slab;
    struct list_head partial_slab_list;
#ifdef SLAB_CRASH_RECOVERY
    struct list_head full_slab_list;
#endif
};
```

**Per-machine CXL slab metadata** (in `dsm_meta`):
```c
// In dsm_meta:
struct {
    struct slab_pointer pool[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];
    struct lock         locks[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];
    struct slab_cpu_log cpu_logs[PLAT_CPU_NUM];
} cxl_slab_meta[CLUSTER_MAX_MACHINE_NUM];
```

When `SLAB_CRASH_RECOVERY` is ON, `cxl_slab.c` uses `dsm_meta->cxl_slab_meta[CUR_MACHINE_ID]` via macros:
```c
#define CXL_SLAB_META  (dsm_meta->cxl_slab_meta[CUR_MACHINE_ID])
#define cxl_slab_pool   (CXL_SLAB_META.pool)
#define cxl_cpu_logs    (CXL_SLAB_META.cpu_logs)
```

### 2.4 Undo Protocol

**Alloc path**:
```
lock()
  slab_log_begin(&cpu_logs[cpu], slab, ALLOC)  // snapshot → FLUSH → FENCE → set op → FLUSH
  slab->free_list_head = next_slot
  slab->current_free_cnt -= 1
  if (full) { choose_new_current_slab(); add to full_slab_list }
  slab_persist_header(slab)                    // FLUSH → FENCE
  slab_log_end(&cpu_logs[cpu])                 // op=NONE → FLUSH
unlock()
```

**Free path**:
```
lock()
  if (was full) { remove from full_slab_list → partial_slab_list }
  slab_log_begin(&cpu_logs[cpu], slab, FREE)
  slot->next_free = slab->free_list_head
  slab->free_list_head = slot
  slab->current_free_cnt += 1
  slab_persist_header(slab)
  slab_log_end(&cpu_logs[cpu])
  if (fully free) return_to_buddy()
unlock()
```

**Optimized persistence**: only 2 `sfence` barriers per operation (log_begin + persist_header). `log_end` skips the fence — the next `log_begin`'s fence orders it.

### 2.5 Crash Semantics

| Crash point | Log state | Recovery action | Outcome |
|---|---|---|---|
| Before log write | `op=NONE` | Nothing | Op never started |
| After log, before mutation | `op=ALLOC/FREE` | Undo | Reverted; no effect |
| After mutation, before log clear | `op=ALLOC/FREE` | Undo | Alloc: slot reclaimed; Free: slot leaked |
| After log clear | `op=NONE` | Nothing | Op completed |

**Safety**: `slab_log_end()` runs before `unlock()`. Caller gets the pointer after `unlock()`. Undo is always safe.

### 2.6 Recovery Flow

`recover_cxl_slabs()` — walks per-CPU logs + pool lists, **no full CXL page scan**:

```
For each machine mid in [0, cluster_machine_num):

  Phase 1: Undo in-flight operations
    For each CPU: check cpu_logs[cpu].op
      If != NONE: restore slab->free_list_head and free_cnt from log, clear op

  Phase 2: Clear all locks
    lock_init() on every locks[cpu][order]

  Phase 3: Rebuild pool lists
    For each (cpu, order) pool:
      Collect all slabs from current_slab + partial_slab_list + full_slab_list
      Re-classify:
        fully free → return to buddy
        full (free_cnt == 0) → full_slab_list
        partial → current_slab or partial_slab_list
```

**Hook point**: called in `ext_mm_init()` (`kernel/mm/mm.c`) in the attach path.

## 3. Files Modified

| File | Change |
|---|---|
| `kernel/include/mm/slab.h` | `slab_cpu_log` struct; `full_slab_list` in `slab_pointer`; per-CPU log helpers |
| `kernel/include/dsm/dsm-single.h` | `cxl_slab_meta[CLUSTER_MAX_MACHINE_NUM]` in `dsm_meta` |
| `kernel/mm/cxl/cxl_slab.c` | Pool/locks/logs via dsm_meta macros; full_slab_list tracking; `recover_cxl_slabs()` |
| `kernel/mm/mm.c` | Call `recover_cxl_slabs()` in attach path |
| `kernel/dsm_config.cmake` | `SLAB_CRASH_RECOVERY` option (default OFF) |
| `kernel/CMakeLists.txt` | Wire cmake variable to compile definition |
| `kernel/tests/tst_slab_recovery.c` | 9 crash recovery tests |
| `kernel/tests/tests.h` / `tests.c` | Test declaration and hookup |

## 4. How to Run

```bash
# Enable: in kernel/dsm_config.cmake, set SLAB_CRASH_RECOVERY "ON"
./quick-build.sh
./dsm-scripts/simulate_ncluster.sh 2 slab_recovery_test.log "echo done" "done"
grep "SLAB_TEST" exec_log0.log
```

## 5. Test Cases

Tests simulate crashes by manually writing partial slab state + per-CPU log, then calling `recover_cxl_slabs()` and verifying `free_list length == current_free_cnt`.

| # | Test | Crash Point | Verifies |
|---|---|---|---|
| 1 | `crash_alloc_before_mutation` | Log written, no mutation | Undo is no-op |
| 2 | `crash_alloc_after_mutation` | Mutation done, log not cleared | Slot returns to free list |
| 3 | `crash_free_before_mutation` | Log written, no mutation | Undo is no-op |
| 4 | `crash_free_after_mutation` | Mutation done, log not cleared | Slot leaked, slab consistent |
| 5 | `no_crash_noop` | No crash | Recovery is no-op |
| 6 | `crash_alloc_multi_slot` | Crash on 17th alloc | Previous 16 slots unaffected |
| 7 | `crash_alloc_partial_mutation` | Only free_list_head updated | Undo fixes inconsistency |
| 8 | `crash_multiple_orders` | All 7 size classes | Recovery handles each order |
| 9 | `post_recovery_functional` | Crash + recovery | Alloc/free work after recovery |

**Result**: ALL 9 TESTS PASSED on both machines (2-machine DSM cluster, QEMU + ivshmem).

## 6. Limitations

1. **Free-crash leak**: undo of a completed free "re-allocates" the slot (bounded: 1 slot per CPU).
2. **DRAM slab not covered**: DRAM content lost on crash; only CXL slabs are instrumented.
3. **One in-flight op per CPU**: sufficient because each CPU holds at most one slab lock.

  What changed from v1 to v2:

  ┌────────────────────┬───────────────────────────────┬──────────────────────────────────┐
  │       Aspect       │       v1 (per-slab log)       │         v2 (per-CPU log)         │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ Log location       │ Embedded in slab_header (CXL) │ Per-CPU in dsm_meta (CXL)        │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ slab_header size   │ Grew by ~16B                  │ Unchanged                        │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ Pool metadata      │ DRAM (cxl_slab_pool global)   │ CXL (dsm_meta->cxl_slab_meta)    │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ Full slab tracking │ Not tracked                   │ full_slab_list added             │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ Recovery method    │ Full CXL page scan            │ Walk N per-CPU logs + pool lists │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ Recovery cost      │ O(CXL pages)                  │ O(CPUs + active slabs)           │
  ├────────────────────┼───────────────────────────────┼──────────────────────────────────┤
  │ sfence per op      │ 3                             │ 2 (commit elides fence)          │
  └────────────────────┴───────────────────────────────┴──────────────────────────────────┘