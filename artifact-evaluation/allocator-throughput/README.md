# Allocator throughput (paper Figure 12)

Reproduces **Figure 12: Memory allocator throughput** — the 3-panel figure
showing (a) Slab (`kmalloc`), (b) Buddy page allocator (mixed random 4 KiB /
2 MiB `get_pages`/`free_pages`), and (c) userspace rpmalloc throughput, each
comparing DRAM against CXL-backed allocation.

## Run

```bash
./artifact-evaluation/prepare.sh                      # once, global
./artifact-evaluation/allocator-throughput/run.sh
```

Runtime: ~1–2 hours with defaults (3 configs × 1 build each × 9 QEMU boots).

## What it does

Three allocator configurations are built and measured:

| Config      | `DSM_CXL_LF_BUDDY` | `SLAB_CRASH_RECOVERY` | `DSM_USER_MALLOC_MODE` |
|-------------|--------------------|-----------------------|------------------------|
| `buddy`     | OFF                | OFF                   | DEFAULT_CXL            |
| `llfree`    | ON                 | OFF                   | DEFAULT_CXL            |
| `llfree_cr` | ON                 | ON                    | DEFAULT_DRAM           |

Kernel-side benchmarks run automatically at boot (`CHCORE_KERNEL_TEST=ON`)
across parallel levels {1,4,8,16,32,48,64,96}; the userspace rpmalloc
benchmark (`malloc_benchmark.bin`) runs in a fresh QEMU boot per thread count.
The `llfree_cr` config doubles as the DRAM baseline for panels (a)/(b)/(c),
matching the paper's data layout.

## Outputs (under `out/<timestamp>/`)

- `logs/` — raw QEMU logs per config/run
- `results/allocator.csv` — kernel-side mean ops/s per (config, memory, test, parallel)
- `results/user-malloc.csv` — userspace rpmalloc per-run rows
- `figures/fig12-allocator-all.pdf` / `.eps` — the paper figure

## Re-parse an existing run

```bash
python3 artifact-evaluation/allocator-throughput/parse_and_plot.py \
  --log-dir artifact-evaluation/allocator-throughput/out/<ts>/logs \
  --out-dir artifact-evaluation/allocator-throughput/out/<ts>
```

## Env knobs

`NRUNS` (default 1), `USER_BENCH_THREADS` (default "1 2 4 8 16 32 64 96"),
`CPU_NUM` (default 96), `CONFIGS`, `TIMEOUT` (default 900 s), `OUT_DIR`.
