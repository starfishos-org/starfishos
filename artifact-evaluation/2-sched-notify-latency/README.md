# Scheduler and notification latency

This artifact runs one two-machine experiment and reports four averages:

- **Local scheduling:** moves a worker from source CPU 0 to another CPU in
  machine 0.
- **Cross-machine scheduling:** moves a worker from machine 0 to machine 1.
- **Local notification:** wakes a waiter on another CPU in machine 0.
- **Cross-machine notification:** wakes a waiter in machine 1.

Scheduling starts immediately before `sys_set_affinity` and ends after the
worker resumes in user mode on the selected CPU. Notification starts
immediately before `sys_notify` and ends after the waiter blocked in `sys_wait`
has resumed in user mode.

Before collecting scheduling samples, the benchmark performs one unreported
warm-up on each destination. This excludes one-time destination page-table
setup while retaining the per-sample affinity change, queueing, interrupt,
scheduling, and user-mode resume costs. Worker stacks and completion state are
explicitly shared so private-page DSM faults are not misreported as scheduler
latency.

The endpoint publishes a completion flag in an explicitly `MAP_FLAG_SHARED`
page (not the process-private heap). A source-machine observer timestamps that
publication, so both timestamps are in the same
monotonic-clock domain.  This avoids treating independently booted QEMU TSCs as
synchronized.  Each guest invocation takes eight samples per metric by default;
the runner repeats the complete experiment three times.  The plot first
averages samples within each run and reports mean ± standard deviation across
runs.

Local and cross-machine notification log lines also report
`notify_syscall_ns` and
`remote_resume_ns`. Their sum is `latency_ns`; the latter covers delivery,
destination scheduling, return to user mode, and completion observation. The
CSV/figure continue to use the end-to-end `latency_ns` value.

The measured scheduler/MSI path must not perform synchronous serial logging.
QEMU serial `printk` can add milliseconds inside the measurement interval and
does not represent interrupt or scheduler latency. Boot-time diagnostics remain
enabled, but per-wakeup timing must be collected in memory rather than printed
from the hot path.

Scheduling uses a fresh migrating thread per sample because this kernel does
not allow an already-bound remote thread to change affinity again.  The batch is
kept deliberately finite so remote detached-thread reclamation cannot exhaust
the current implementation; independent runner repetitions provide 24 samples
by default.  Notification reuses one persistent remote waiter for its batch.

## Run the complete evaluation

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
```

Useful overrides:

```bash
NRUNS=3 SAMPLES=8 LOCAL_CPU=4 REMOTE_CPU=12 \
  ./artifact-evaluation/2-sched-notify-latency/run.sh
```

CPU IDs are global ChCore CPU IDs. `LOCAL_CPU` must belong to machine 0 and
differ from source CPU 0; `REMOTE_CPU` must belong to machine 1.

The current checked-in smoke-test result (`SAMPLES=4`, one VM run, local CPU 4,
cross-machine CPU 16) is:

| Metric | Average latency |
| --- | ---: |
| Local sched | 19.410 µs |
| Cross-machine sched | 65.192 µs |
| Local notify | 15.310 µs |
| Cross-machine notify | 22.843 µs |

These four-sample values verify the path and scripts; use the default three
runs or a larger `SAMPLES` value for reported experimental results.

By default, `run.sh` writes the raw logs, parsed CSV files, and figure directly
to `artifact-evaluation/2-sched-notify-latency/`. No output-directory setting is
needed.

By default, the runner rebuilds ChCore with `./quick-build.sh` so the
microbenchmark and current kernel changes are present in the image.
`SKIP_BUILD=1` is only an
advanced shortcut when the existing image is known to match the current source
and configuration.

`run.sh` boots a fresh two-machine cluster for each run, collects the raw logs,
then invokes `plot.py`. `plot.py` parses the sample lines into `samples.csv`
and `summary.csv` before drawing the figure.

## Regenerate results and figures only

To parse the existing default `logs/` directory and redraw the figure without
rebuilding or booting QEMU:

```bash
python3 artifact-evaluation/2-sched-notify-latency/plot.py
```

By default, the generated CSV files and figure are written in this directory.
`--log-dir` and `--out-dir` can override those locations.

## Run the single-kernel Linux reference

`run_linux.sh` measures the same user-visible endpoints between two CPUs on one
host Linux kernel. Scheduling starts immediately before a worker changes its
CPU affinity and ends after it resumes on the destination CPU. Notification
starts immediately before an `eventfd` write and ends after the destination
waiter returns to user mode. This is a local cross-core reference; it is not a
Linux cross-machine transport measurement.

One-click `run_all.py` runs this automatically after the ChCore/QEMU
`run.sh` when the `sched-notify` experiment is selected:

```bash
python3 artifact-evaluation/run_all.py --experiments-only sched-notify
# or standalone:
./artifact-evaluation/2-sched-notify-latency/run_linux.sh
```

The script automatically selects the first two CPUs allowed by the process.
CPUs and sample counts can also be selected explicitly:

```bash
SOURCE_CPU=0 REMOTE_CPU=12 SAMPLES=1000 NRUNS=3 \
  ./artifact-evaluation/2-sched-notify-latency/run_linux.sh
```

Linux logs, CSV summaries, and the figure are written to `linux-results/` in
this directory by default. Set `OUT_DIR` to override it. Gather copies the
Linux figure as `linux_sched_notify_latency.png` under
`out/<ts>/figures/sched-notify/`. The test uses `CLOCK_MONOTONIC_RAW`, pins
the source observer and destination waiter to different CPUs, and does not
require root privileges.

## Outputs

The output location is this evaluation directory:

- `logs/runN/machine0.log`, `logs/runN/machine1.log`: raw logs;
- `samples.csv`: parsed per-sample values;
- `summary.csv`: separate statistics for `local_sched`, `cross_sched`,
  `local_notify`, and `cross_notify`;
- `sched_notify_latency.png`: generated figure.
