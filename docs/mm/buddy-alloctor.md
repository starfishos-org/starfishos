  实现总结                                                                                                                                                
                                                                                                                                                          
  文件结构                                                                                                                                                
                                                                                                                                                          
  ┌─────────────────────────────────┬─────────────────────────────────────────────────────────────┐                                                       
  │              文件               │                            作用                             │                                                       
  ├─────────────────────────────────┼─────────────────────────────────────────────────────────────┤                                                       
  │ kernel/include/dsm/dsm_ref.h    │ 类型定义 + API 声明                                         │                                                       
  ├─────────────────────────────────┼─────────────────────────────────────────────────────────────┤                                                       
  │ kernel/dsm/dsm_ref.c            │ 实现（~200 行）                                             │
  ├─────────────────────────────────┼─────────────────────────────────────────────────────────────┤                                                       
  │ kernel/include/dsm/dsm-single.h │ 在 dsm_metadata_t 末尾加 dsm_ref_meta_t ref_meta（~4.8 KB） │
  └─────────────────────────────────┴─────────────────────────────────────────────────────────────┘                                                       
                                                            
  核心数据结构（存在 CXL SHM）                                                                                                                            
                                                            
  dsm_ref_meta_t (在 dsm_meta 里，所有 machine 共享可见)                                                                                                  
   ├── era[8][8]               256 B  — era[i][j] = machine i 见到的 machine j 的最大 era                                                                 
   └── machine[8]              4608 B                                                                                                                     
        ├── mid_token (u64)    — 0=空闲; machine_id+1=已加入                                                                                              
        └── redo[8]            — 8 条 redo log 条目（ring，按 cur_era & 7 索引）                                                                          
             ├── func_id       LINK / UNLINK                                                                                                              
             ├── saved_cnt     CAS 前的 ref_cnt                                                                                                           
             ├── cur_era       操作时刻的 era                                                                                                             
             ├── ref_loc_off   被更新的指针字段在 SHM 中的 offset                                                                                         
             └── refed_off     目标对象在 SHM 中的 offset                                                                                                 
                                                                                                                                                          
  操作顺序（与论文一致）

  dsm_ref_link:
  1. 读 ref_info → 提取 (lcid, cnt, saw_era)
  2. 更新 era[mid][lcid]（若 saw_era 更新则写入 + smp_mb）
  3. 写 redo log → smp_mb          ← crash 后 recovery 的入口
  4. CAS ref_info: cnt → cnt+1, lcid=mid, era=cur_era
  5. *ref_loc = target_off          ← crash 在这里是可 redo 的
  6. era[mid][mid]++

  dsm_ref_recover_machine(dead_mid) — era-based 三分法：
  - Case A：obj->ref_info.lcid == dead_mid && era == redo->cur_era → CAS 提交了，只需补写指针
  - Case B：era[j][dead_mid] >= redo->cur_era（有 live machine 见过这个 era）→ 同上
  - Case C：两者都不满足 → CAS 根本没提交，无需任何操作

---

## CXL SHM Memory Management: Design and Implementation

### Overview

The CXL shared memory (SHM) allocator in ChCore-CXL consists of two complementary layers, each targeting a distinct allocation granularity and failure-safety requirement.

### Layer 1: Lock-Free Page Allocator (Buddy System)

**Origin.** The page-granularity allocator for CXL SHM is a C port of a lock-free buddy allocator originally implemented in Rust (`linux-tests/lock_free_buddy_allocator/`). The underlying algorithm is due to Pellegrini (2017) [^pellegrini], which organizes a binary tree of memory regions whose nodes are updated exclusively through hardware compare-and-swap (CAS) instructions; no mutex or spinlock is held across the allocation critical section.

**Integration.** The C port lives in `kernel/mm/buddy-lock-free.c`. When both `USE_CXL_MEM` and `DSM_CXL_LF_BUDDY` are defined, `buddy_get_pages()` dispatches CXL-typed pool requests to `buddy_lf_get_pages()`. Tree metadata (node array and container array) is placed directly inside the CXL SHM window so that all machines share a single allocator state via CAS.

**Failure properties.** Because no blocking primitive is held across an allocation, a machine crash cannot prevent surviving machines from making progress in the allocator. A crash during an in-flight allocation may leave at most one tree node in a transiently-locked state; these stuck nodes are reclaimed by a post-crash scan after the dead machine is confirmed via heartbeat timeout, without any modification to the allocation hot path.

### Layer 2: Slab Allocator (Sub-page Objects)

For allocations up to 2 KB, `cxl_kmalloc()` delegates to `alloc_in_cxl_slab()` (`kernel/mm/cxl/cxl_slab.c`), which maintains per-size-class slab caches backed by pages drawn from the lock-free buddy above. The slab layer is protected by conventional spinlocks (`dsm_meta->slabs_locks[]`) and is **not** partial-failure resilient; it is retained because (i) the target workloads allocate large, long-lived graph structures that bypass the slab, and (ii) replacing the slab lock with a crash-safe variant is deferred to future work.

### Layer 3: Per-Machine Redo-Log + Era Reference Counting

For kernel objects placed in CXL SHM that require lifetime management across machine boundaries, we implement a reference-counting layer (`kernel/dsm/dsm_ref.c`) directly inspired by CXL-SHM [^cxlshm], simplified from per-thread to per-machine granularity under the whole-machine failure assumption.

**Failure model.** We assume fail-stop semantics at machine granularity: when a failure occurs, all CPUs on the affected machine halt simultaneously. We do not consider partial CPU failures within a machine.

**ref_info encoding.** Each reference-counted object carries a 64-bit `ref_info` field packed as `[lcid:16 | cnt:16 | era:32]`, where `lcid` identifies the machine that last modified the field, `cnt` is the current reference count, and `era` is `lcid`'s monotonically increasing era counter at the time of the modification.

**Era matrix.** A shared `era[N][N]` matrix (N ≤ 8 machines; 256 bytes total) records, for each pair (i, j), the latest era of machine j observed by machine i. Before every CAS on `ref_info`, the observer updates `era[i][lcid]` if the era embedded in the old `ref_info` is newer. This matrix is the key witness used by recovery to determine whether an in-flight CAS committed.

**Redo log.** Each machine maintains a ring buffer of eight redo-log entries in CXL SHM (one 64-byte cache line each). Before issuing a CAS on `ref_info`, the operating machine writes `{op, saved_cnt, cur_era, ref_loc_off, refed_off}` and issues an `mfence`, ensuring the log entry is globally visible before the CAS is attempted.

**Normal operation (link).** The sequence for incrementing a reference is:
1. Read `ref_info`; extract `(lcid, cnt, era)`.
2. Update `era[mid][lcid]` if `saw_era` is newer (store + `mfence`).
3. Write redo-log entry; issue `mfence`.
4. CAS `ref_info`: `pack(lcid, cnt, era) → pack(mid, cnt+1, cur_era)`.
5. Store `*ref_loc = target_off`.
6. Increment own era.

**Recovery (era-based three-way check).** When machine M is confirmed dead, the survivor locates M's highest-era redo entry and applies one of three cases, identical to CXL-SHM §4.3:
- *Case A*: `obj->ref_info` shows `lcid == M` and `era == redo.cur_era`. The CAS committed but the pointer store (step 5) may have been lost; re-apply the store.
- *Case B*: `era[j][M] ≥ redo.cur_era` for some live machine j. Machine j observed M's modification; the CAS committed; re-apply the pointer store.
- *Case C*: Neither A nor B holds. The CAS never committed; no state was changed; no action required.

After recovery, M's slot (`mid_token`) is cleared atomically.

**Complexity reduction vs. CXL-SHM.** By restricting the failure granularity to whole machines, we eliminate per-thread thread-local state (TLS) stored in SHM, the RootRef/TBR garbage-collection root mechanism, and the O(N²) era matrix scaled to thread count. The era matrix shrinks from up to 256 × 256 × 4 = 256 KB (for 1024 threads) to 8 × 8 × 4 = 256 bytes. The redo-log pool shrinks from one per thread to one per machine. Recovery does not require walking a per-thread root set; a single redo entry per machine suffices.

[^pellegrini]: A. Pellegrini, "A Scalable Lock-Free Buddy Allocator," 2017. https://alessandropellegrini.it/publications/tScar17.pdf
[^cxlshm]: Y. Zhang et al., "Partial Failure Resilient Memory Management System for (CXL-based) Distributed Shared Memory," 2023.