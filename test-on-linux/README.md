# test-on-linux

Linux (bare-metal) baselines used by the paper's **Ideal** and **Distributed**
curves. Ports mirror `chcore-cxl/test-on-linux/`; each entry is a git submodule
on the Linux-side branch (distinct from the ChCore ports under `user/demos/`).

## Init

```bash
git submodule update --init --recursive test-on-linux
```

## Layout vs paper figures

| Path | Role in paper / AE |
| --- | --- |
| `phoenix/` | Ideal for auto-scale Matrix Multiply (and related MapReduce) |
| `dbx1000/` | Ideal for auto-scale / DBx1000 Linux DRAM |
| `GeminiGraph/` | Ideal (LINUX-DRAM) single-node GeminiGraph |
| `ggraph-distri/` | Distributed / Linux-MPI multi-node GeminiGraph |
| `leveldb/` | Linux LevelDB baseline (when needed) |
| `linux-kernel-test/` | Host Linux microbenchmarks (`nvsys/FITS/linux-tests`) |

Sched/notify host Linux microbench for AE lives separately at
`artifact-evaluation/2-sched-notify-latency/run_linux.sh` (not a submodule).

## Notes

- ChCore app ports stay under `user/demos/` (different branches/remotes).
- AE `6-auto-scale` is still a stub; wire runners here when implementing Ideal.
- Local-only dirs from chcore-cxl (e.g. `mapreduce/`, `fxmark/`, `lmbench`)
  are not vendored here; keep them outside the repo or add later if needed.
