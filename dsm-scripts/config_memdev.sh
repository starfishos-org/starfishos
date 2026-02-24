#!/bin/bash

mode=$1
memNumaNode=4
size=32 # 32GB shared memory
numa_size=12
devName="/dev/shm/ivshmem-$USER"
hostfsDevName="/dev/shm/ivshmem-hostfs-$USER"
hostfsSize=16

# Create 8 CXL device files (numa0.0 to numa3.1), each 16G
numa_devs=(
  "/dev/shm/numa0.0-$USER"
  "/dev/shm/numa1.0-$USER"
  "/dev/shm/numa2.0-$USER"
  "/dev/shm/numa3.0-$USER"
  "/dev/shm/numa0.1-$USER"
  "/dev/shm/numa1.1-$USER"
  "/dev/shm/numa2.1-$USER"  
  "/dev/shm/numa3.1-$USER"
)

if [ "$mode" = "cxl-new" ]; then
  # Remove old shared memory
  rm -rf $devName
  rm -rf $hostfsDevName
  echo "Old Shared Memory and Hostfs Removed"

  # Remove old CXL device files
  for dev in "${numa_devs[@]}"; do
    rm -rf $dev
  done
  echo "Old CXL Device Files Removed"

  # Create 8 CXL device files, each 16G
  for i in {0..7}; do
    dev_path=${numa_devs[$i]}
    numactl --membind=$(($i / 2)) dd if=/dev/zero of=$dev_path bs=1G count=$numa_size
    echo "Created CXL device $i: $dev_path (16G)"
  done

  # Create shared memory (ivshmem) for CXL shared memory
  numactl --membind=$memNumaNode dd if=/dev/zero of=$devName bs=1G count=$size
  dd if=/dev/zero of=$hostfsDevName bs=1G count=$hostfsSize
  echo "New Shared Memory (on NUMA $memNumaNode) Malloced"
fi

python3 dsm-scripts/prepare_cxlmem.py
