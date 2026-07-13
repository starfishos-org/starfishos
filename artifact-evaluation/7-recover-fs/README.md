# 7 — LevelDB filesystem recovery

This evaluation produces the recovery timeline used by the final paper figure.
It does not simulate a crash by terminating the guest command: it kills the
entire machine-0 QEMU window. A test-local host detector watches the actual
machine-0 QEMU PID and records when it exits. Machine 1 remains alive, starts a replacement
filesystem instance with `tmpfs.srv --recover 0`, and then reopens the same
LevelDB database with `--use_existing_db=1`.

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/7-recover-fs/run.sh
```

The script builds by default.  Use `SKIP_BUILD=1` only if the existing image
already includes `leveldb-dbbench.bin` and the tmpfs recovery instrumentation.

## Outputs

`out/<timestamp>/` contains raw per-machine QEMU logs plus:

- `recovery_detail.csv`: host wall-clock stage durations and guest-reported
  filesystem/p-log/LevelDB DB::Open times;
- `throughput.csv`: actual pre-crash and post-recovery db_bench measurements;
- `recovery-performance.png` and `.pdf`: paper-style recovery timeline.

Useful overrides include `FILL_NUM`, `READ_NUM`, `THREADS`, `CRASH_DELAY`,
`TIMEOUT`, `OUT_DIR`, and `KEEP_QEMU=1`.  The database path defaults to
`/tmp/leveldb_recovery`.

To redraw a completed run without booting QEMU:

```bash
python3 artifact-evaluation/7-recover-fs/plot.py \
  --detail artifact-evaluation/7-recover-fs/out/<timestamp>/recovery_detail.csv \
  --throughput artifact-evaluation/7-recover-fs/out/<timestamp>/throughput.csv \
  --out-dir artifact-evaluation/7-recover-fs/out/<timestamp>
```
