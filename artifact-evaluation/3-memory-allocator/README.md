# Memory allocator evaluation

This artifact evaluates the paper's CXL memory allocator configurations:

- **Buddy:** original lock-based CXL buddy allocator, crash recovery off
- **LLFree:** lock-free CXL page allocator, crash recovery off
- **LLFree+CR:** lock-free CXL page allocator with slab crash recovery on

For each configuration it builds a fresh image and runs:

- kernel `kmalloc` throughput on DRAM and CXL;
- 4 KiB and 2 MiB `get_pages` / `free_pages` throughput;
- mixed random 4 KiB + 2 MiB allocation/free throughput;
- user-space malloc throughput over a thread-count sweep.

The original `kernel/dsm_config.cmake` is restored automatically on normal
exit, error, or interruption.

The AE entry point itself sets `DSM_CXL_LF_BUDDY` and
`SLAB_CRASH_RECOVERY`, enables `CHCORE_KERNEL_TEST` and
`CHCORE_BUILD_USER_MALLOC_TESTS`, performs a fresh build for each row of the
configuration matrix, and only then launches that configuration's QEMU runs.
It deliberately uses `chbuild clean` plus `chbuild build`; `quick-build.sh` is
not used because its `defconfig` step would reset the AE-selected options.

## Run the complete evaluation

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/3-memory-allocator/run.sh
```

The full sweep is intentionally long. Useful overrides are:

```bash
NRUNS=3 ./artifact-evaluation/3-memory-allocator/run.sh
USER_BENCH_THREADS="1 2 4 8 16" ./artifact-evaluation/3-memory-allocator/run.sh
CPU_NUM=96 ./artifact-evaluation/3-memory-allocator/run.sh
CLEAN_RESULTS=0 ./artifact-evaluation/3-memory-allocator/run.sh
```

`run.sh` runs the benchmarks, parses the raw logs into
`allocator_results.csv`, and then invokes `plot.py`. Unlike the other two
evaluations, allocator log parsing currently lives in `run.sh`; `plot.py` only
loads the resulting CSV and draws the figure.

## Outputs

- `logs/`: one fixed raw-log directory (no timestamp/checkpoint directories)
- `allocator_results.csv`: all measured throughput rows
- `fig00-allocator-all.png`: p3os-paper-style Slab/Buddy/rpmalloc figure

The default is one run per configuration. This produces 27 useful raw logs:
one kernel log plus eight user-thread logs for each of the three allocator
configurations. Set `NRUNS=3` only when error bars across repeated runs are
needed. Redundant `.match` checkpoint files are not generated.

## Regenerate the figure only

To redraw the figure from the existing CSV without rebuilding or booting QEMU:

```bash
python3 artifact-evaluation/3-memory-allocator/plot.py
```

By default, the script reads `allocator_results.csv` and writes the figure in
this directory.  `--csv` and `--out-dir` can override those locations.
