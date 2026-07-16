#!/bin/bash

mode=$1
memNumaNode=4
size=64 # 64GB shared memory
numa_size=16 # Default size of each numax.x (GB); can be overridden by numa_sizes.conf

# Large CXL-related ivshmem still lives under /mnt/cxlshm
base_dir="/dev/shm"
devName="$base_dir/ivshmem-$USER"
hostfsDevName="$base_dir/ivshmem-hostfs-$USER"

# Per-NUMA device files numax.x are placed under /dev/shm
numa_base_dir="/dev/shm"
hostfsSize=16

project_root="$(cd "$(dirname "$0")/.." && pwd)"
ini_loader="$project_root/scripts/common/load_chcore_ini.sh"

size_to_gib_count() {
  local s="$1"
  local n unit bytes gib
  n="${s%[KkMmGg]}"
  unit="${s#$n}"
  if [ "$n" = "$s" ]; then
    echo "$s"
    return
  fi
  case "$unit" in
    G|g) echo "$n" ;;
    M|m)
      bytes=$(( n * 1024 * 1024 ))
      gib=$(( (bytes + 1024*1024*1024 - 1) / (1024*1024*1024) ))
      echo "$gib"
      ;;
    K|k)
      bytes=$(( n * 1024 ))
      gib=$(( (bytes + 1024*1024*1024 - 1) / (1024*1024*1024) ))
      [ "$gib" -lt 1 ] && gib=1
      echo "$gib"
      ;;
    *) echo "$size" ;;
  esac
}

if [ -f "$ini_loader" ]; then
  # shellcheck source=/dev/null
  . "$ini_loader"
  load_chcore_ini
  if [ -n "$CHCORE_INI_CXL_SIZE" ]; then
    size=$(size_to_gib_count "$CHCORE_INI_CXL_SIZE")
  fi
fi

# Load per-NUMA config if present
numa_sizes_conf="$(dirname "$0")/numa_sizes.conf"
if [ -f "$numa_sizes_conf" ]; then
  # shellcheck source=/dev/null
  . "$numa_sizes_conf"
fi

# Create 8 CXL device files (numa0.0 to numa3.1), each 16G
numa_devs=(
  "$numa_base_dir/numa0.0-$USER"
  "$numa_base_dir/numa0.1-$USER"
  "$numa_base_dir/numa1.0-$USER"
  "$numa_base_dir/numa1.1-$USER"
  "$numa_base_dir/numa2.0-$USER"
  "$numa_base_dir/numa2.1-$USER"
  "$numa_base_dir/numa3.0-$USER"
  "$numa_base_dir/numa3.1-$USER"
)

new_numa() {
  # remove old CXL device files
  for dev in "${numa_devs[@]}"; do
    rm -rf $dev
  done
  echo "Old NUMA Device Files Removed"
  
  # Create 8 NUMA device files; size can be set per numax.x
  for i in {0..7}; do
    dev_path=${numa_devs[$i]}
    # Size (GB) for this numax.x: prefer NUMA_SIZES[i], else fall back to global numa_size
    per_numa_size="${NUMA_SIZES[$i]}"
    if [ -z "$per_numa_size" ]; then
      per_numa_size=$numa_size
    fi

    numactl --membind=$(($i / 2)) dd if=/dev/zero of="$dev_path" bs=1G count="$per_numa_size"
    echo "Created NUMA device $i: $dev_path (${per_numa_size}G) (bind on NUMA $(($i / 2)))"
  done
}

new_cxl() {
  # remove old shared memory
  rm -rf $devName
  echo "Old Shared Memory Removed"

  # Create shared memory (ivshmem) for CXL shared memory
  numactl --membind=$memNumaNode dd if=/dev/zero of=$devName bs=1G count=$size
  echo "New Shared Memory (on NUMA $memNumaNode) Malloced"
}

new_hostfs() {
  # remove old hostfs
  rm -rf $hostfsDevName
  echo "Old Hostfs Removed"
  
  # create hostfs
  dd if=/dev/zero of=$hostfsDevName bs=1G count=$hostfsSize
  echo "New Hostfs Malloced"
}

if [ "$mode" = "numa-new" ]; then
  new_numa
fi

if [ "$mode" = "cxl-new" ]; then
  new_cxl
fi

if [ "$mode" = "hostfs-new" ]; then
  new_hostfs;
  python3 dsm-scripts/prepare_hostfs.py;
fi

if [ "$mode" = "new-all" ]; then
  new_numa;
  new_cxl;
  new_hostfs;
  python3 dsm-scripts/prepare_hostfs.py;
fi

# CXLFS persists the boot ramdisk across runs.  Recreate the device when the
# built ramdisk no longer matches the stamped image (e.g. after rebuild).
"$project_root/dsm-scripts/prepare_cxlfs_dev.sh" ensure

python3 dsm-scripts/prepare_cxlmem.py
