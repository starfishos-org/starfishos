mode=$1

rm -rf "/dev/shm/ivshmem-$USER"

if [ "$mode" = "cxl" ]; then
    # sudo daxctl reconfigure-device --mode=system-ram dax0.0 --force
    numactl --membind=2 dd if=/dev/zero of="/dev/shm/ivshmem-$USER" bs=1G count=4
fi