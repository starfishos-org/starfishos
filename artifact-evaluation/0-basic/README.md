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

## Intel MLC bandwidth

The wrapper automatically discovers the installed MLC binary at
`/home/wfn/MLC-Linux/mlc` or
`/home/wfn/shm-pcc-sdk/tests/basic/global_tests/mlc`:

```bash
./artifact-evaluation/0-basic/run_mlc.sh
```

It runs both `--bandwidth_matrix` and `--peak_injection_bandwidth`. Set
`MODE=matrix` or `MODE=peak` to run one mode, `MLC_BIN` to select another MLC
installation, and `OUT_DIR` to change the output directory. `-e` is used so MLC
does not attempt to modify hardware prefetcher state.
