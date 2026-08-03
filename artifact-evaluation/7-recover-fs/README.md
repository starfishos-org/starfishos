# LevelDB filesystem recovery (paper Figure 16)

Kills machine-0 QEMU, recovers CXLFS on machine 1, and reopens the same LevelDB
database to measure recovery timeline and post-crash throughput.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/7-recover-fs/run.sh
```

To run only the machine crash and boot-time cluster rejoin regression:

```bash
./artifact-evaluation/7-recover-fs/run-rejoin.sh
```

To run a Phoenix matrix process across both machines, kill its remote
participant, rejoin that machine ID, and verify task/resource reclamation:

```bash
./artifact-evaluation/7-recover-fs/run-cross-task-rejoin.sh
```

To keep a client application alive while its remote service machine fails,
verify that the in-flight request returns `-ECONNABORTED`, and then reconnect
to the replacement service:

```bash
./artifact-evaluation/7-recover-fs/run-ipc-abort-rejoin.sh
```

This regression leaves a DurableQueue request in `DOING` on machine 1, kills
that QEMU, and rejoins logical machine 1. The replacement polling service
preserves the CXL queue and publishes `ABORT` for the ambiguous request. The
application on machine 0 must observe `-ECONNABORTED`; it does not replay the
request automatically, remaps the service queue, and verifies a new request.

The cross-task command follows the launch conventions used by AE 5: it writes
a global CPU binding file and starts one multithreaded process on machine 0.
The shell suffix `# &` marks it cross-machine and runs it in the background.
The test waits until a worker is recorded on machine 1, kills machine 1,
restarts logical ID 1 at generation 2, and requires both the partial-failure
and full capability-reclamation log markers on the surviving owner.

The rejoin test keeps machine 1 and the CXL/ivshmem service alive, kills
machine 0's QEMU, then starts a replacement QEMU with logical machine ID 0.
It verifies that the replacement observes boot generation 2, attaches to the
existing CXL allocators and durable queues, rebuilds its volatile kernel state,
reaches a shell prompt, and does not interrupt machine 1.

## Outputs

Each run creates `artifact-evaluation/7-recover-fs/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | Machine logs, detector output, and machine 0/1 rejoin logs. |
| `csv/` | `recovery_detail.csv`, `throughput.csv` |
| `figures/` | `recovery-performance-single.png` — paper Figure 16 |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 7
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/7-recover-fs/plot.py \
  --detail artifact-evaluation/7-recover-fs/out/<timestamp>/csv/recovery_detail.csv \
  --throughput artifact-evaluation/7-recover-fs/out/<timestamp>/csv/throughput.csv \
  --fig-dir artifact-evaluation/7-recover-fs/out/<timestamp>/figures
```

## Env knobs

`FILL_NUM`, `READ_NUM`, `THREADS`, `CRASH_DELAY`, `TIMEOUT`, `SKIP_BUILD`,
`KEEP_QEMU`, `BOOT_ONLY`, `REJOIN_ONLY`, `CROSS_TASK_ONLY`, `IPC_ABORT_ONLY`,
`CROSS_MATRIX_SIZE`, `USE_DEV_AS_DRAM`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`,
`FIG_DIR`, `TS`.
