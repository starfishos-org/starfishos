# 06 — Paper figure map (SOSP'26 submission ↔ AE experiments)

Authoritative mapping between the **figure numbers the SOSP'26 reviewers saw**,
the LaTeX labels in `/mnt/disk1/yjs/p3os-paper`, and the artifact-evaluation
experiments that regenerate them.

**Always quote reviewer feedback against the submitted numbering in the first
table.** The paper working tree has already drifted (see "Numbering drift"
below), and an earlier internal review round used yet another numbering, so
"Figure N" is ambiguous unless the source is stated.

## Submitted numbering (what the reviews refer to)

| Submitted | LaTeX label | Source | Content | AE experiment |
| --- | --- | --- | --- | --- |
| Fig. 11 | `fig:eval-ipc` | `8-eval.tex` | (a) IPC latency CDF, (b) read-4KiB latency breakdown | `1-ipc-cdf` |
| Fig. 12 | `fig:eval-page-alloc` | `8-eval.tex` | memory allocator throughput | `3-memory-allocator` |
| **Fig. 13** | `fig:eval-state-partition` | `8-eval.tex` | **state-partition ablation (submitted at 2 machines)** | `4-state-partition` |
| Fig. 14 | `fig:eval-auto-scale` | `8-eval.tex` | auto-scaling apps (matrix / DBx1000 / GeminiGraph) | `5-auto-scale` |
| Fig. 15 | `fig:eval-resource-utilization` | `8-eval.tex` | resource utilization | `6-resource-util` |
| Fig. 16 | `fig:eval-recovery-performance` | `8-eval.tex` | LevelDB recovery after a machine failure | `7-recover-fs` |
| Table 3 | — | `8-eval.tex` | CXL latency/bandwidth/MSI setup | `0-basic` |
| Table 4 | — | `8-eval.tex` | per-app memory footprint (8 machines) | `5-auto-scale` footprint pass |
| §8.2 text | — | `8-eval.tex` | scheduling/notification latency | `2-sched-notify-latency` |

Camera-ready additions (no submitted figure number yet):

| AE experiment | Answers | Content |
| --- | --- | --- |
| `8-dbx1000-cross-warehouse` | Reviewer B Q3 | TPC-C cross-warehouse ratio sweep (15/50/80%) |
| `9-queue-saturation` | Reviewer B on Fig. 11(b) | per-service-queue tail latency + saturation throughput |

## Numbering drift — do not use the working tree's numbers

The working tree of `7-applications.tex` added design figures (migration,
multi-page-table) after submission, so the eval figures shifted by three:
`fig:eval-ipc` is now **Figure 14** in a local build, `fig:eval-state-partition`
is **Figure 16**, and so on. Regenerate the mapping with a document-order walk
over the `\input` list in `starfish.tex` before trusting any local number.

A third numbering exists: the internal pre-submission review in
`p3os-paper/review.txt` calls the state-partition ablation "图11 (Figure 11)".
That is the same plot as submitted Fig. 13 — it is *not* about the IPC figure.

## Machine counts requested by reviewers

| Ask | Figure | Where | Status |
| --- | --- | --- | --- |
| "only 2 machines … show at 4 and 8 machines" | **Fig. 13** (state partition) | Reviewer B, details list | `4-state-partition` defaults to `MACHINE_COUNTS="4 8"`; §8.3 text still says "across two machines" (`8-eval.tex:329`) |
| "report tail latency and saturation throughput per service queue" | Fig. 11(b) | Reviewer B, details list | `9-queue-saturation`; **no machine-count change was requested** — it stays a 2-machine client/service pair |
| "why is remote IPC 24 µs / queueing so large" | Fig. 11(b) | Reviewer E | same experiment |
| "workload that does not partition cleanly" | new | Reviewer B Q3 | `8-dbx1000-cross-warehouse` |
| 8-machine IPC variant (8-machine cluster, m1→m0 single sender + 4 concurrent senders) | Fig. 11 | **internal request, not reviewer text** | not implemented; `1-ipc-cdf/run.sh:13` hardcodes `NUM_MACHINES=2` |

The last row is the one that most often gets confused with Reviewer B's Fig. 13
ask, because both are phrased as "2 machines should be 8". They are different
figures and different experiments: Fig. 13 has an explicit reviewer request and
`4-state-partition` already supports it; the 8-machine IPC variant of Fig. 11 is
an internal wish with no script support yet.

## Memory-placement configurations

Figure comparisons only line up when the DSM placement matches. The four
`kernel/dsm_config.cmake` combinations used across experiments:

| Paper label | `DSM_MALLOC_MODE` | `DSM_USER_MALLOC_MODE` | Used by |
| --- | --- | --- | --- |
| Share (All_CXL) | `CXL` | `DEFAULT_CXL` | `4-state-partition` |
| K-mix/U-share | `MIXED_DEFAULT_DRAM` | `DEFAULT_CXL` | `4-state-partition` |
| K-mix/U-mix | `MIXED_DEFAULT_DRAM` | `DEFAULT_DRAM` | `4-state-partition` |
| Private (All_DRAM) | `DRAM` | `DEFAULT_DRAM` | `4-state-partition` baseline |
| StarfishOS-Mixed (Fig. 14) | `MIXED_DEFAULT_CXL` | `DEFAULT_CXL` | `5-auto-scale` |
| StarfishOS-CXL (Fig. 14) | `CXL` | `DEFAULT_CXL` | `5-auto-scale` |

`DSM_USER_MALLOC_MODE` is what decides where a user heap page lands
(`kernel/mm/kmalloc.c:139` for `kmalloc`, `:190` for `get_pages`):
`DEFAULT_CXL` sends `__MT_USER_DEFAULT__` straight to CXL, `DEFAULT_DRAM` keeps
it in the first-touching machine's DRAM so that only cross-machine access pulls
it into CXL through the page-fault migration path of §7.2.
