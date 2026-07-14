# Basic platform measurements

This directory contains low-level measurements used to separate platform and
virtualization costs from higher-level ChCore paths.

## ivshmem MSI delivery latency

The source kernel timestamps immediately before writing the ivshmem doorbell.
The target MSI handler writes a shared completion sequence before message
parsing or scheduler work, and the source stops timing when it observes that
sequence. Thus the result excludes migration, notification, remote scheduling,
and return to user mode.

Use this number as the transport baseline for
`2-sched-notify-latency`: its cross-machine results additionally include shared
queue handling, destination scheduling, and return to user mode. Neither test
prints synchronously from the measured MSI wake-up path.

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/0-basic/run_msi.sh
```

The default is 100 deliveries from machine 0 to machine 1 local CPU 4. Override
it with `SAMPLES`, `TARGET_MACHINE`, `TARGET_CPU`, or `NRUNS`. The runner builds
with `quick-build.sh` by default; use `SKIP_BUILD=1` only for a matching image.
Raw logs go to `msi-logs/`; parsed results are `msi_samples.csv` and
`msi_summary.csv` in this directory by default.

## Intel MLC bandwidth (paper Table 1 / host Linux)

Host-side DRAM/CXL bandwidth for the evaluation setup table. One-click
`run_all.py` always invokes this after MSI when running the `basic` experiment.

The wrapper looks for `mlc` on `PATH`, or uses `MLC_BIN` if set:

```bash
MLC_BIN=/path/to/mlc ./artifact-evaluation/0-basic/run_mlc.sh
# or via one-click:
python3 artifact-evaluation/run_all.py --experiments-only basic
```

It runs both `--bandwidth_matrix` and `--peak_injection_bandwidth`. Set
`MODE=matrix` or `MODE=peak` to run one mode, and `OUT_DIR` to change the
output directory. If MLC is missing, the script skips by default
(`ALLOW_MLC_SKIP=1`); set `ALLOW_MLC_SKIP=0` to fail instead. `-e` is used so
MLC does not attempt to modify hardware prefetcher state. One-click reports
`OK[MLC-OK]`, `OK[MLC-SKIPPED]`, or `FAILED(...MLC-FAILED...)` in the status.
