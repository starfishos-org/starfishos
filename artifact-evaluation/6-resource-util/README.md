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

`run.sh` enables and rebuilds Redis, Memcached/Memcachetest, and TinyCNN
automatically, then restores the original build configuration. TinyCNN is the
historical pinned submodule and currently requires access to the IPADS GitLab.
Legacy demos are materialized into clean out-of-tree build directories, so
ignored host objects are neither reused nor written back into their submodules.
`prepare_cnn.sh` creates deterministic zero-weight AlexNet data and an 8-image
batch; this preserves the inference compute/memory path, while model accuracy
is irrelevant to this performance-only figure. TinyCNN is linked with libjpeg
and decodes JPEG inputs in-process, avoiding CImg's external converter path and
the process-related syscalls that path would require from the guest.
DBx1000 is likewise rebuilt with the compact read-only YCSB configuration used
by this figure, rather than inheriting the large TPC-C auto-scale settings.

## Run

```bash
./artifact-evaluation/prepare.sh                 # once
./artifact-evaluation/6-resource-util/run.sh
```

Every successful collection writes `out/<timestamp>/results/real.csv`; the
default full run also writes `out/<timestamp>/figures/real.{eps,pdf,png}`.

Env overrides: `STRESS_TYPES` (default `1 2 3 4 5 6`), `CONDS`
(`single stress p3os`), `SINGLE_BENCHES` (default `matrix leveldb
linear-regression dbx1000 pca redis word-count memcached gemini string-match
kmeans cnn`), `TIMEOUT`, and `OUT_DIR`. The three scope variables are
whitespace-separated lists. Unknown or duplicate entries, embedded newlines,
and empty selections are rejected before the runner takes its lock or changes
host/repository state.

For example, this collects only GeminiGraph's single-run value:

```bash
CONDS=single SINGLE_BENCHES=gemini \
    ./artifact-evaluation/6-resource-util/run.sh
```

Known serial-console-silent workloads, including LevelDB, use `TIMEOUT` as
their progress fail-safe while the runner continues to scan for fatal guest
signatures and dead tmux panes.

## Re-plot only / verify against paper data

The drawing logic is copied verbatim from `p3os-paper/eval/real.py` and is
validated — it reproduces `real.eps` from the paper's own CSV:

```bash
python3 artifact-evaluation/6-resource-util/plot.py \
    --csv /mnt/disk1/yjs/p3os-paper/eval/real.csv \
    --out-dir /tmp/real-check
```

## Completeness contract

The default path requires all 36 values (12 applications × 3 conditions), even
when each complete scope list is reordered. A legal scope subset automatically
uses partial plotting while still requiring every point that the runner
requested; a missing or unparseable requested metric fails the run. Stress and
cross-machine runs wait for each member of the selected type pair independently;
fixed sleeps are no longer used.

`results/real.csv` always preserves the real metrics produced by a successful
subset. Normalization and a figure are produced only for benchmarks that have
all three conditions. If (for example) a single-only run has no complete
triplet, the plotter emits an explicit warning, skips the figure, and succeeds;
it also removes stale `real.eps`, `real.pdf`, and `real.png` files if an output
directory is reused, and never invents the missing normalization inputs. Direct
plotter use can add repeatable `--require-point <bench>-<cond>` checks.
`--allow-partial` remains a debug/subset option; without it the plotter enforces
the full 36-point dataset.
