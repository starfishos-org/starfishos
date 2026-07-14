# 6 — Auto-scaling applications (paper Fig auto-scale)

**Status: stub** — entry script not implemented yet.

Target paper figures (`p3os-paper/eval/`):

- `auto-scale-matrix.eps`
- `db1000.eps`
- `gemini-chcore.eps`

## Intended workflow

1. Sweep machine counts (1 → 8) under Mixed / DRAM configs.
2. Run Matrix Multiply (Phoenix), DBx1000, GeminiGraph via existing
   `dsm-scripts/tests/` expect scripts / Makefile targets.
3. Write CSVs matching `p3os-paper/eval/` schema.
4. Plot with logic adapted from:
   - `p3os-paper/eval/auto_scale_matrix.py`
   - `p3os-paper/eval/db1000.py`
   - `p3os-paper/eval/gemini_graph.py`

## Run (once implemented)

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/6-auto-scale/run.sh
```

Until then, `run-all.sh` reports this experiment as `STUB(not implemented)`.
