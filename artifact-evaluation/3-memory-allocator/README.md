# Memory allocator (paper Figure 12)

Evaluates Buddy, LLFree, and LLFree+CR allocator configurations on DRAM and CXL.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/3-memory-allocator/run.sh
```

Useful overrides: `NRUNS`, `USER_BENCH_THREADS`, `CPU_NUM`.

## Outputs

Each run creates `artifact-evaluation/3-memory-allocator/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | Kernel and user benchmark raw logs |
| `csv/` | `allocator_results.csv` |
| `figures/` | `allocator-all.png` — paper Figure 12 |

`run.sh` parses logs into `csv/allocator_results.csv`; `plot.py` draws the
paper figure.

## Re-plot only

```bash
./artifact-evaluation/run-all.sh --plot-only --run-subset-of-tests 3
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

`NRUNS`, `RUN_OFFSET`, `USER_BENCH_THREADS`, `CPU_NUM`, `BUILD_ONLY`,
`OUT_DIR`, `LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`.

`BUILD_ONLY=1` builds all three configurations and runs none of them, which
answers "do Buddy, LLFree and LLFree+CR all still compile" in six builds
instead of an afternoon of boots. It writes no CSV and no figure.

```bash
BUILD_ONLY=1 ./artifact-evaluation/3-memory-allocator/run.sh
```
