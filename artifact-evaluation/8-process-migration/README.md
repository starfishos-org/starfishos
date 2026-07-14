# 8 — Process migration breakdown (paper Fig process-migration)

Reproduces the checkpoint/restore (cfork) time-breakdown figures:

- `process-migration-data-large.eps` — prepare stages, in **ms**
- `process-migration-data-small.eps` — stop/ckpt/restore stages, in **µs**

## How it works

`CHCORE_SSI_SLS=ON` (default in this tree) enables the cfork checkpoint path.
For each benchmark, `run.sh`:

1. Boots a 2-machine cluster.
2. Starts the workload on machine 0.
3. Runs `test_cfork_prepare.bin <binary>` on machine 0 — checkpoints the
   process into the shared KVS and prints the **prepare/checkpoint** breakdown
   and per-object copy times (`perf_cfork_time[...]`,
   `prepare copy time object: ...`).
4. Runs `test_cfork_restore.bin <binary>` on machine 1 — restores it and prints
   the **restore** breakdown (`perf_restore_time[...]`).
5. Concatenates both machine logs to `logs/<Benchmark>.log`.

`parse_and_plot.py` parses those lines into `results/process-migration.csv`
(same columns as `p3os-paper/eval/process-migration.csv`) and draws both
figures. The timing print is unconditional (`PERF_TIMING_CFORK` in
`kernel/include/dsm/perf_timing.h`) — no special rebuild needed.

Default benchmark rows (paper order): `float linpack matmul pyaes pca db1000`.
These need the cfork test binaries and each workload's `.bin` in hostfs
(`test_cfork_prepare.bin`, `test_cfork_restore.bin`, `test_float.bin`,
`test_linpack.bin`, `test_matmul.bin`, `test_pyaes.bin`, `pca.bin`, `rundb.bin`).

## Run

```bash
./artifact-evaluation/prepare.sh                 # once
./artifact-evaluation/8-process-migration/run.sh
```

Output: `out/<timestamp>/figures/process-migration-data-{large,small}.{eps,pdf,png}`.

Env overrides: `BENCHES`, `NUM_MACHINES` (default 2), `TIMEOUT` (default 120),
`OUT_DIR`.

## Re-plot only / verify against paper data

The drawing logic is copied verbatim from `p3os-paper/eval/process_migration.py`.
Verify it reproduces the paper figure from the paper's own CSV:

```bash
python3 artifact-evaluation/8-process-migration/parse_and_plot.py \
    --csv /mnt/disk1/yjs/p3os-paper/eval/process-migration.csv \
    --out-dir /tmp/pm-check
```

## Caveats

The per-benchmark ready/prepare/restore log markers in `run.sh` mirror
`dsm-scripts/tests/{config,cfork_prepare,cfork_restore}.exp`. They have not
been validated against a live cfork run in this tree — if a workload uses a
different completion line, adjust `bench_ready_marker` / `*_DONE` accordingly.
