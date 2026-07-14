# 9 — Resource utilization / co-location (paper Fig real)

Reproduces `real.eps`: 12 applications, each measured under three conditions
and normalized to its own single-run.

- `single` — the application alone on one machine (baseline)
- `stress` — co-located with a competing workload, traditional DRAM placement
- `p3os`  — co-located under StarfishOS (the competitor offloaded to a second
  machine over CXL/DSM)

The 12 apps: LevelDB, DBx1000, Matrix Mult., Linear Reg., PCA, Word Count,
String Match, KMeans (Phoenix), GeminiGraph, Redis, Memcached, CNN.

## How it works

The co-location pairs are already encoded in
`user/script/single_stress_type{1..6}.sh` (traditional, one machine) and
`user/script/cross_stress_type{1..4}_m{0,1}.sh` (StarfishOS, two machines),
and are driven manually by `dsm-scripts/tests/real/{dram,cxl}.sh`. `run.sh`
wraps that in the AE harness, captures per-condition logs, then
`plot.py` extracts each app's metric into `results/real.csv` (same
schema as `p3os-paper/eval/real.csv`) and draws the figure.

## Prerequisites

`user/demos/config.cmake`: the paper set needs `LEVELDB`, `PHOENIX`, `DBX1000`,
`GEIMINIGRAPH` (currently ON) plus `REDIS`, `MEMCACHED`/`MEMCACHETEST`,
`TINYCNN` (currently OFF — enable and rebuild for a full run).

## Run

```bash
./artifact-evaluation/prepare.sh                 # once
./artifact-evaluation/9-resource-util/run.sh
```

Output: `out/<timestamp>/figures/real.{eps,pdf,png}`.

Env overrides: `STRESS_TYPES` (default `1 2 3 4 5 6`), `CONDS`
(`single stress p3os`), `TIMEOUT`, `OUT_DIR`.

## Re-plot only / verify against paper data

The drawing logic is copied verbatim from `p3os-paper/eval/real.py` and is
validated — it reproduces `real.eps` from the paper's own CSV:

```bash
python3 artifact-evaluation/6-resource-util/plot.py \
    --csv /mnt/disk1/yjs/p3os-paper/eval/real.csv \
    --out-dir /tmp/real-check
```

## Status / caveats

Plotting is done and validated. **Data collection is scaffolded but not yet
validated against a live co-location run**:

- the single-run baselines are left as a documented gap in `run.sh` (each app
  needs its own invocation + completion marker);
- the stress/p3os runs reuse the existing stress scripts, but the mapping from
  each per-stress-type log to a per-application `<bench>_<cond>.log`, and the
  per-app metric extractors in `plot.py`, must be confirmed against
  real application output before the numbers are trustworthy.
