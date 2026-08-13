
# Prepare

## prepare hostfs

```bash
python prepare_hostfs.py
```
Will copy all files in `source_file_list` to shared memory.

add files to `source_file_list`

Extra files can also be supplied without editing the script. Separate multiple
paths with `:` on Linux; relative paths are resolved from the repository root:

```bash
CHCORE_HOSTFS_FILES=/path/to/model.gguf python dsm-scripts/prepare_hostfs.py
```

## prepare memory device (simulate CXL memory)

Allocate a new memory device by: 

```bash
./dsm-scripts/config_memdev.sh cxl
```

- `mode=$1`: `cxl` or `cxl-new`
- `size=64`: the size of the memory device (GB)
- `devName="/dev/shm/ivshmem-$USER"`: the name of the memory device
- `CXL_MEM_POLICY` / `CXL_MEM_NODES` (env, default `interleave` across `4,5`):
  host NUMA placement of the shared CXL region. It is interleaved over both
  memory-only nodes rather than pinned to one, because a single CXL node sits
  much closer to one socket than to the others and can introduce
  socket-dependent performance.
  Requested nodes that do not exist on the host are dropped automatically,
  falling back to `--membind=4` and then to the default policy.

# Run

To run, make sure we have the tmux installed.

Run the program by:

```bash
# simulate 2 clusters
./dsm-scripts/simulate_2clusters.sh 
# or 
./dsm-scripts/simulate_4clusters.sh
```

To stop the window, you can use:

```bash
tmux kill-session -t mywork
```

or add the following line in `~/.tmux.conf`, so you can kill the whole tmux session by `C-q`.

```text
bind -n C-q kill-session
```


Besides, if the start window number might mismatch, you can change the `window_start_index` in `simulate_2clusters.sh` or `simulate_4clusters.sh` to the correct number.

# Host-side log watchdog

`log_watchdog.py` runs on the **host**, next to a test rather than inside QEMU.
It tails every machine's `exec_log<N>.log` and, as soon as a fatal guest
signature appears (kernel panic, `BUG:`, protection fault, unhandled exception,
…), writes a flag file and exits non-zero with the offending line and the
context around it.

`simulate_ncluster.sh` (and therefore every `make run-*-test` target) starts one
automatically for the machines it boots and checks the flag in each of its wait
loops, so a run aborts about a second after a guest dies instead of sitting
until its timeout. `SIM_LOG_WATCHDOG=0` turns it off. The artifact-evaluation
scripts do the same through `ae_start_log_watchdog` in
`artifact-evaluation/common.sh` (`AE_LOG_WATCHDOG=0` to disable).

The expect-driven tests (`make leveldb`, `make pca`, `make gemini`, …) run
through `run_with_watchdog.sh`, which starts a watchdog for the command and
kills the test — and reaps its QEMU — when the watchdog fires:

```bash
./dsm-scripts/run_with_watchdog.sh -n 2 -- ./dsm-scripts/tests/phoenix/pca.exp 8
```

Opt out with `make <target> WATCHDOG_RUN=` or `WATCHDOG=0 make <target>`; `-k`
keeps the guests alive after a detected failure for debugging.

Attach the watchdog by hand to any other run:

```bash
./dsm-scripts/log_watchdog.py --log-dir logs --count 2 --flag-file /tmp/wd.flag
```

Useful options: `--machines 1,2` (watch a subset, e.g. after deliberately
killing a machine), `--start-at-end` (ignore what a log already contains),
`--ignore REGEX` (repeatable, for known-benign lines), `--kill-pid PID` (signal
the launcher on detection).
