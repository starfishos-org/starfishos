mode=$1

echo 0 > /proc/sys/kernel/numa_balancing
echo "NUMA Balancing Disabled"

rm -rf "/dev/shm/ivshmem-$USER"
echo "Old Shared Memory Removed"

if [ "$mode" = "cxl" ]; then
    # sudo daxctl reconfigure-device --mode=system-ram dax0.0 --force
    numactl --membind=2 dd if=/dev/zero of="/dev/shm/ivshmem-$USER" bs=1G count=4
    echo "New Shared Memory (on CXL) Malloced"
fi
