# Memory allocator (paper Figure 12)

Evaluates Buddy, LLFree, and LLFree+CR allocator configurations on DRAM and CXL.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/3-memory-allocator/run.sh
```

The default is three independent runs. User rpmalloc sweeps 1--96 threads on
a 96-vCPU guest, performs a 10% warmup, then executes 50000 measured loops per
thread.

Useful overrides: `NRUNS`, `USER_BENCH_THREADS`, `USER_BENCH_LOOPS`, `CPU_NUM`.

## Outputs

Each run creates `artifact-evaluation/3-memory-allocator/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | Kernel and user benchmark raw logs |
| `csv/` | `allocator_results.csv` |
| `figures/` | `allocator-all.png` — filtered-mean paper Figure 12; `allocator_summary.csv` — mean/std/variance and filtering counts |

`run.sh` parses logs into `csv/allocator_results.csv`; `plot.py` draws the
paper figure. Groups with at least five samples drop near-zero failed runs,
keep the densest cluster when CV is still high (contaminated secondary mode),
then apply a modified-Z-score polish (median/MAD, threshold 3.5) before the
plotted mean; the summary records the raw, retained, and excluded sample counts.

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 3
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/3-memory-allocator/plot.py \
  --csv artifact-evaluation/3-memory-allocator/out/<timestamp>/csv/allocator_results.csv \
  --fig-dir artifact-evaluation/3-memory-allocator/out/<timestamp>/figures
```

Paper CSV validation example:

```bash
python3 artifact-evaluation/3-memory-allocator/plot.py \
  --csv /path/to/paper/allocator.csv \
  --user-csv /path/to/paper/user-malloc.csv \
  --fig-dir /tmp/allocator-check/figures
```

## Env knobs

`NRUNS`, `RUN_OFFSET`, `USER_BENCH_THREADS`, `USER_BENCH_LOOPS`, `CPU_NUM`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`.

`AE_UNBIND_DRAM_NUMA=1` — recreate machine-0 DRAM (`/dev/shm/numa0.0-*`) spread
across host CPU nodes 0–3 (1 GiB chunk rotation; plain `numactl --interleave`
is ignored by this host's tmpfs). Other `numa*.*` files are sparse placeholders
(this AE only boots machine 0). (`=off` uses plain `dd`, which often still
lands on one node.) Restored to membind-0 on exit. CXL `ivshmem` stays on its
configured memory node.
