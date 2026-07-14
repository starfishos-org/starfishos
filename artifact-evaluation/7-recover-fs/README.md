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

- `recovery_detail.csv`: host wall-clock stage durations and guest-reported
  filesystem/p-log/LevelDB DB::Open times;
- `throughput.csv`: actual pre-crash and post-recovery Read/Fill db_bench
  measurements in long format (`event,elapsed_ms,workload,ops_per_sec`);
- `recovery-performance-single.png`, `.pdf`, and `.eps`: the paper-compatible
  11 x 3.2 inch recovery figure. Compatibility copies without `-single` are
  also emitted as PNG/PDF.

Raw QEMU and detector logs are written separately to `logs/` as
`machine0.log`, `machine1.log`, and `machine0-detector.log`. Every run
overwrites these files, so `logs/` keeps only the latest run.

Useful overrides include `FILL_NUM`, `READ_NUM`, `THREADS`, `CRASH_DELAY`,
`TIMEOUT`, `OUT_DIR`, `LOG_DIR`, `USE_DEV_AS_DRAM`, and `KEEP_QEMU=1`.
`OUT_DIR` changes the CSV/figure destination, while `LOG_DIR` changes the raw
log destination. The evaluation defaults `USE_DEV_AS_DRAM=1` for the current
device-backed-DRAM build and passes it directly to each QEMU launch. It also
restarts the test's ivshmem doorbell server to reset stale peer IDs. The
database path defaults to `/tmp/leveldb_recovery`.

`FILL_NUM` defaults to 128 and defines the populated/read key space.
`READ_NUM` defaults to 1000 and controls the number of random read samples per
db_bench thread (8000 samples with the default eight threads).
The p-log is a 4 MiB uncommitted redo tail; committed CXL filesystem state is
checkpointed and the tail is truncated automatically. The test does not keep
or restore a second tmpfs image and does not reset the log before crashing.

To redraw a completed run without booting QEMU:

```bash
python3 artifact-evaluation/7-recover-fs/plot.py
```

The input and output paths shown above are the plotter defaults; all three can
still be overridden with `--detail`, `--throughput`, and `--out-dir`.
