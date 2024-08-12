mode=$1
memNumaNode=5
devName="/dev/shm/ivshmem-$USER"

rm -rf $devName
echo "Old Shared Memory Removed"

if [ "$mode" = "cxl" ]; then
	# sudo daxctl reconfigure-device --mode=system-ram dax0.0 --force
	numactl --membind=$memNumaNode dd if=/dev/zero of=$devName bs=1G count=4
	echo "New Shared Memory (on NUMA $memNumaNode) Malloced"
fi
