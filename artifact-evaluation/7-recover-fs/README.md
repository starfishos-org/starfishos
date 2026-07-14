# 7 — LevelDB filesystem recovery

This evaluation produces the recovery timeline used by the final paper figure.
It does not simulate a crash by terminating the guest command: it kills the
entire machine-0 QEMU window. A test-local host detector watches the actual
machine-0 QEMU PID and records when it exits. Machine 1 remains alive, starts a replacement
filesystem instance with `cxlfs.srv --recover 0`, and then reopens the same
LevelDB database with `--use_existing_db=1`. The run measures both workloads:
`readrandom` for Read, and LevelDB writes for Fill (`fillbatch` before the
crash, `overwrite` on the recovered existing database).

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/7-recover-fs/run.sh
```

The script builds by default.  Use `SKIP_BUILD=1` only if the existing image
already includes `leveldb-dbbench.bin` and the tmpfs recovery instrumentation.

## Outputs

Generated CSV files and figures are written directly to
`artifact-evaluation/7-recover-fs/` using stable names:

- `recovery_detail.csv`: host wall-clock stage durations, guest-reported
  filesystem/p-log/LevelDB DB::Open times, and p-log replay entry/error counts;
- `throughput.csv`: actual pre-crash and post-recovery Read/Fill db_bench
  measurements in long format (`event,elapsed_ms,workload,ops_per_sec`);
- `recovery-performance-single.png`, `.pdf`, and `.eps`: the crash-relative
  11 x 3.2 inch recovery figure. It plots recovered read throughput from the
  LevelDB failure at `t=0`; compatibility copies without `-single` are also
  emitted as PNG/PDF.

Raw QEMU and detector logs are written separately to `logs/` as
`machine0.log`, `machine1.log`, and `machine0-detector.log`. Every run
overwrites these files, so `logs/` keeps only the latest run.

Useful overrides include `FILL_NUM`, `READ_NUM`, `THREADS`, `CRASH_DELAY`,
`TIMEOUT`, `OUT_DIR`, `LOG_DIR`, `USE_DEV_AS_DRAM`, and `KEEP_QEMU=1`.
`OUT_DIR` changes the CSV/figure destination, while `LOG_DIR` changes the raw
log destination. The evaluation defaults `USE_DEV_AS_DRAM=0`: ordinary guest
DRAM uses QEMU RAM, while CXLFS uses its dedicated 8 GiB ivshmem device. It also
restarts the test's ivshmem doorbell server to reset stale peer IDs. The
database path defaults to `/tmp/leveldb_recovery`.

`FILL_NUM` defaults to 128 and defines the populated/read key space.
`READ_NUM` defaults to 10000 and `THREADS` defaults to 1. The recovery curve
uses the second of two `readrandom` passes: the first warms the recovered SST
and block cache, while the second reports steady-state single-thread read
throughput. `fillbatch` only populates the database before the crash and is not
used as the recovered-read throughput.
The p-log is a 4 MiB uncommitted redo tail; committed CXL filesystem state is
checkpointed and the tail is truncated automatically. The test does not keep
or restore a second tmpfs image and does not reset the log before crashing.

To redraw a completed run without booting QEMU:

```bash
python3 artifact-evaluation/7-recover-fs/plot.py
```

The input and output paths shown above are the plotter defaults; all three can
still be overridden with `--detail`, `--throughput`, and `--out-dir`.
