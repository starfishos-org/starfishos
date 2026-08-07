#!/usr/bin/env bash
# Recreate guest DRAM backing for the memory-allocator AE with controlled host
# NUMA placement. Only numa0.0 (machine 0 DRAM) is fully populated; the other
# numa*.* files are sparse 16GiB placeholders so /dev/shm pressure stays low.
#
# Usage:
#   recreate_numa_dram.sh bind         # membind numa0.0 → host node 0 (default layout)
#   recreate_numa_dram.sh interleave   # interleave numa0.0 across CPU nodes 0-3
#   recreate_numa_dram.sh off          # plain dd (often still one host node)
set -euo pipefail

MODE="${1:-bind}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NUMA_BASE="/dev/shm"
SIZE_GIB="${NUMA_DRAM_SIZE_GIB:-16}"
INTERLEAVE_NODES="${NUMA_DEV_INTERLEAVE_NODES:-0,1,2,3}"

NUMA_NAMES=(
  numa0.0 numa0.1
  numa1.0 numa1.1
  numa2.0 numa2.1
  numa3.0 numa3.1
)

if pgrep -u "$USER" -f 'qemu-6\.2-system-x86_64.*chcore-' >/dev/null 2>&1; then
  echo "[numa-dram] refusing: ChCore QEMU still holds numa files" >&2
  exit 1
fi

echo "[numa-dram] mode=$MODE size=${SIZE_GIB}G"
for name in "${NUMA_NAMES[@]}"; do
  rm -f "$NUMA_BASE/$name-$USER"
done

# Fully populate machine-0 DRAM only.
primary="$NUMA_BASE/numa0.0-$USER"
case "$MODE" in
  bind)
    numactl --membind=0 dd if=/dev/zero of="$primary" bs=1G count="$SIZE_GIB" status=progress
    echo "[numa-dram] created $primary (membind=0)"
    ;;
  interleave)
    # tmpfs often ignores process --interleave and collapses onto one node.
    # Write 1 GiB chunks with rotating --membind across CPU nodes instead.
    IFS=',' read -r -a nodes <<< "$INTERLEAVE_NODES"
    nnodes=${#nodes[@]}
    : > "$primary"
    for ((chunk = 0; chunk < SIZE_GIB; chunk++)); do
      node="${nodes[$((chunk % nnodes))]}"
      numactl --membind="$node" \
        dd if=/dev/zero of="$primary" bs=1G seek="$chunk" count=1 status=none conv=notrunc
    done
    echo "[numa-dram] created $primary (chunk-rotated membind across $INTERLEAVE_NODES)"
    ;;
  off|0)
    dd if=/dev/zero of="$primary" bs=1G count="$SIZE_GIB" status=progress
    echo "[numa-dram] created $primary (no membind)"
    ;;
  *)
    echo "unknown mode: $MODE (expected bind|interleave|off)" >&2
    exit 2
    ;;
esac

# Sparse placeholders for the remaining ivshmem-plain DRAM devices.
for name in "${NUMA_NAMES[@]}"; do
  [ "$name" = "numa0.0" ] && continue
  path="$NUMA_BASE/$name-$USER"
  truncate -s "${SIZE_GIB}G" "$path"
  echo "[numa-dram] sparse placeholder $path"
done

python3 "$REPO_ROOT/dsm-scripts/prepare_cxlmem.py"
echo "[numa-dram] done"
