# 6 — Auto-scaling applications (paper Fig auto-scale)

Reproduces three figures, each throughput/runtime vs machine count (1–8):

- `auto-scale-matrix.eps` — Matrix-multiply MapReduce
- `db1000.eps` — DBx1000 TPC-C
- `gemini-chcore.eps` — GeminiGraph PageRank

Each compares StarfishOS **Mixed** (`MIXED_DEFAULT_CXL`) and **CXL** (all-CXL)
against external baselines.

## How it works

`run.sh` sweeps the machine count under both StarfishOS placements, booting an
N-machine cluster per point and recording each run's time/throughput — these
are the StarfishOS curves.

The other curves are **external baselines that this script does not run**:

| Baseline | Source (`test-on-linux/`) | Appears in |
|---|---|---|
| Ideal (Linux DRAM) — Matrix | `phoenix` | matrix (IDEAL) |
| Ideal (Linux DRAM) — DBx1000 | `dbx1000` | db1000 (linux) |
| Ideal (Linux DRAM) — Gemini | `GeminiGraph` | gemini (LINUX-DRAM) |
| Distributed (Linux-MPI) — Gemini | `ggraph-distri` | matrix/gemini (TCP / DISTRIBUTED) |
| Tigon (external DB) — DBx1000 | not vendored here | db1000 (tigon) |

See `test-on-linux/README.md`. Their numbers are merged into the data files
`plot.py` consumes.

## Data-file formats (identical to `p3os-paper/eval/`)

- matrix: lines `RESULT: N=<m> CONFIG=<MIXED|CXL|TCP|IDEAL> TIME=<us>`
- db1000: CSV `module,machines,performance_mops` (modules `P3OS-mixed`,
  `P3OS-all_cxl`, `tigon`, `linux`)
- gemini: CSV `machines,MIXED_DEFAULT_CXL,CXL,DRAM,LINUX-DRAM,DISTRIBUTED`
  (values `<seconds>s`)

## Run

```bash
./artifact-evaluation/prepare.sh                 # once
./artifact-evaluation/5-auto-scale/run.sh
```

Output: `out/<timestamp>/figures/{auto-scale-matrix,db1000,gemini-chcore}.{eps,pdf,png}`.

Env overrides: `APPS` (`matrix db1000 gemini`), `MACHINES` (`1 2 4 6 8`),
`CONFIGS` (`Mixed CXL`), `TIMEOUT`, `OUT_DIR`.

## Re-plot only / verify against paper data

The drawing logic is copied verbatim from `p3os-paper/eval/{auto_scale_matrix,
db1000,gemini_graph}.py` and is validated — it reproduces all three figures
from the paper's own data files:

```bash
python3 artifact-evaluation/5-auto-scale/plot.py --out-dir /tmp/as-check \
  --matrix-data /mnt/disk1/yjs/p3os-paper/eval/mapreduce/4000size.txt \
  --db1000-data /mnt/disk1/yjs/p3os-paper/eval/db1000/db1000-p3os-tigon.csv \
  --gemini-data /mnt/disk1/yjs/p3os-paper/eval/gemini_graph/data.log
```

## Status / caveats

Plotting is done and validated. **Data collection is scaffolded but not yet
validated against a live sweep**: the Mixed/CXL config mapping, per-app
multi-machine invocation, completion markers, and the conversion of run logs
into the data-file formats above must be confirmed against an executed sweep,
and the external Linux/Tigon baselines produced separately and merged in.
