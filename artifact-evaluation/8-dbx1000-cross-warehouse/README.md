# DBx1000 cross-warehouse sweep (reviewer request)

Sweeps the fraction of **cross-warehouse TPC-C transactions**
(`PERC_REMOTE_PAYMENT` / `PERC_REMOTE_NEW_ORDER`) on an 8-machine cluster and
reports:

- **Throughput** (`thp=` — Mtxn/s)
- **CXL / local-DRAM memory footprint** (from the kernel's
  `[VMSPACE MEMORY]` summaries, enabled via `PRINT_VMSPACE_STATS`; dbx1000
  calls `usys_print_vmspace_stats()` after init, warmup, and execution)

This is the experiment requested by the review: how does performance and CXL
usage change as the cross-warehouse ratio varies.

## Run

```bash
./artifact-evaluation/prepare.sh                          # once, global
./artifact-evaluation/5-dbx1000-cross-warehouse/run.sh
```

Runtime: ~1–3 hours (3 ratios × 1 build × one 8-machine QEMU cluster run;
the vendored dbx1000 config is the 16 GB-scale TPC-C setup with warmup, so a
single run takes tens of minutes).

## What it does

Per ratio R ∈ {15, 50, 80}:

1. `config.h`: `PERC_REMOTE_PAYMENT = PERC_REMOTE_NEW_ORDER = R`,
   `NUM_MACHINES` synced to the cluster size.
2. Kernel built with `PRINT_VMSPACE_STATS` (+`_NO_DETAILS`) enabled.
3. Boot 8 machines (guest DRAM `DRAM_SIZE=24G` — the 16 GB-scale tables do
   not fit in the default 16G), run `rundb.bin` with threads bound across all
   machines, wait for `PASS! SimTime`.
4. Archive all 8 machine logs.

All modified files (`config.h`, `kernel/CMakeLists.txt`, `.config`,
`kernel/dsm_config.cmake`) are restored on exit.

## Outputs (under `out/<timestamp>/`)

- `logs/machine<i>_r<R>.log`
- `results/cross_warehouse.csv` — ratio, thp, CXL MB, DRAM MB, CXL/ALL %
- `results/footprint_per_machine.csv` — per-machine detail
- `figures/dbx1000-cross-warehouse.pdf` / `.eps` — (a) throughput and
  (b) footprint vs ratio

## Re-parse an existing run

```bash
python3 artifact-evaluation/8-dbx1000-cross-warehouse/plot.py \
  --log-dir artifact-evaluation/5-dbx1000-cross-warehouse/out/<ts>/logs \
  --out-dir artifact-evaluation/5-dbx1000-cross-warehouse/out/<ts> \
  --num-machines 8 --ratios 15 50 80
```

## Env knobs

`RATIOS` (default "15 50 80"), `NUM_MACHINES` (default 8), `DRAM_SIZE`
(default 24G), `TIMEOUT` (default 3600 s), `OUT_DIR`.

## Background

Methodology and previous measured results (thp 2.10/1.79/1.68 at 15/50/80%,
CXL ≈ 800 MB steady) are recorded in
`chcore-cxl/docs/CXL_FOOTPRINT_DIAGNOSIS_PLAN.md`.
