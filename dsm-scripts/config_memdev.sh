mode=$1
memNumaNode=4
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
	python3 dsm-scripts/prepare_cxlmem.py
fi
