# Paper figures checklist

Refreshed by a real (non-`DRY_RUN`) `./artifact-evaluation/run-all.sh`.
Per-run details also live in `out/<timestamp>/FIGURES.md`.

## Ready (implemented — produced by default one-click)

| Experiment | Paper figure | Outputs |
| --- | --- | --- |
| `1-ipc-cdf` | IPC CDF + breakdown | `ipc_cdf.png`, `ipc_read_breakdown.png` |
| `3-memory-allocator` | allocator | `fig00-allocator-all.{png,pdf,eps}` |
| `4-state-partition` | state partition | `fig13-state-partition.{pdf,eps}` |
| `7-recover-fs` | recovery timeline | `recovery-performance.{png,pdf}` |

## TODO (not implemented yet)

- [ ] **auto-scale** (`6-auto-scale/`)
  - paper: `auto-scale-matrix.eps`, `db1000.eps`, `gemini-chcore.eps`
  - reuse: `dsm-scripts/tests/{db1000,gemini}.exp`, phoenix; plot from `p3os-paper/eval/`
- [ ] **process-migration** (`8-process-migration/`)
  - paper: `process-migration-data-large.eps`, `process-migration-data-small.eps`
  - reuse: `dsm-scripts/tests/cfork_*.exp`, `ckpt_*.exp`; note SSI/TreeSLS may be gated off
- [ ] **resource-util** (`9-resource-util/`)
  - paper: `real.eps`
  - reuse: `dsm-scripts/tests/real/`; plot from `p3os-paper/eval/real.py`

## One-click

```bash
python3 artifact-evaluation/run_all.py   # first run also does make-prepare build if needed
```
