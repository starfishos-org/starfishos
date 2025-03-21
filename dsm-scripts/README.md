
# Prepare

## prepare hostfs

```bash
python prepare_hostfs.py
```
Will copy all files in `source_file_list` to shared memory.

add files to `source_file_list`

## prepare memory device (simulate CXL memory)

Allocate a new memory device by: 

```bash
./dsm-scripts/config_memdev.sh cxl
```

- `mode=$1`: `cxl` or `cxl-new`
- `memNumaNode=5`: the numa node to allocate memory
- `size=32`: the size of the memory device
- `devName="/dev/shm/ivshmem-$USER"`: the name of the memory device

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

