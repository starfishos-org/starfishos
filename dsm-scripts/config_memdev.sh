#!/bin/bash

mode=$1
memNumaNode=4
size=32 # 32GB shared memory
numa_size=16 # 默认每个 numax.x 的大小（GB），可被 numa_sizes.conf 覆盖

# CXL 相关大块 ivshmem 仍放在 /mnt/cxlshm
base_dir="/mnt/cxlshm"
devName="$base_dir/ivshmem-$USER"
hostfsDevName="$base_dir/ivshmem-hostfs-$USER"

# 单个 NUMA 设备文件 numax.x 改放在 /dev/shm 下
numa_base_dir="/dev/shm"
hostfsSize=16

# 如果存在 per-numa 配置，则加载
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
  
  # create 8 NUMA device files, size 可按 numax.x 单独配置
  for i in {0..7}; do
    dev_path=${numa_devs[$i]}
    # 取得该 numax.x 的大小（GB），优先使用 NUMA_SIZES[i]，否则回退到全局 numa_size
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

python3 dsm-scripts/prepare_cxlmem.py
