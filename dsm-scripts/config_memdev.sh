mode=$1
memNumaNode=5
size=32 # 64GB shared memory
devName="/dev/shm/ivshmem-$USER"

if [ "$mode" = "cxl-new" ]; then
	rm -rf $devName
	echo "Old Shared Memory Removed"
	# sudo daxctl reconfigure-device --mode=system-ram dax0.0 --force
	numactl --membind=$memNumaNode dd if=/dev/zero of=$devName bs=1G count=$size
	echo "New Shared Memory (on NUMA $memNumaNode) Malloced"
fi

if [ "$mode" = "cxl" ]; then
	numactl --membind=$memNumaNode dd if=/dev/zero of=$devName bs=1024 count=4 conv=notrunc
	echo "Set first 4KB of Shared Memory (on NUMA $memNumaNode) to 0"
fi
