# 6 — Resource utilization / co-location (paper Fig real)

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
`user/script/cross_stress_type{1..6}_m{0,1}.sh` (StarfishOS, two machines;
same pairs, one app per machine), and are driven manually by
`dsm-scripts/tests/real/{dram,cxl}.sh`. `run.sh`
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
./artifact-evaluation/6-resource-util/run.sh
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

`run.sh` now (1) runs per-app **single** baselines into `<bench>_single.log`,
(2) demuxes stress/p3os type bundles into `<bench>_{stress,p3os}.log`, and
(3) calls `plot.py --log-dir`. Plotting skips benches missing any of the three
conditions. Cross scripts cover all six paper pairs (types 1–6); **live
co-location numbers are still unvalidated** against a full run.
