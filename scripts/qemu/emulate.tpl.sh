#!/bin/bash

set -e

vm_id=0
machine_id=0
if [ -n "$1" ]; then
	vm_id=$1
	machine_id=$1
else
	machine_id=-1
fi
basedir=$(dirname "$0")
# basedir should be /build directory
nvm_backend_file="/tmp/nvm-file-$USER"
memdev_dir="/mnt/cxlshm"
ivshmem_dev="$memdev_dir/ivshmem-$USER"
conn_size=16G
ivshmem_conn_dev="$memdev_dir/ivshmem-conn-$USER"
hostfs_size=16G
ivshmem_hostfs_dev="$memdev_dir/ivshmem-hostfs-$USER"
dram_size=1G # 1G for temp allocator
numa_size=16G # 默认大小，仅在对应文件不存在时兜底
cxl_size=32G # 默认大小，仅在对应文件不存在时兜底
plat_cpu_name=12
# ivshmem_dev="/dev/dax0.0,align=2M"
# align=2M: refer https://docs.pmem.io/persistent-memory/getting-started-guide/creating-development-environments/virtualization/qemu#nvdimm-io-alignment

# pass a virtio file to the qemu
virtio_file_name="$basedir/../disk.img"
# echo "virtio_file_name: $virtio_file_name"

port=$(shuf -i 30000-40000 -n 1)
#while true; do
#	port=\$(shuf -i 30000-40000 -n 1)
#	netstat -tan | grep \$port > /dev/null 2>&1
#	if [[ \$? -ne 0 ]]; then
#		break
#	fi
#done

echo $port >$basedir/gdb-port-$vm_id
# echo $port >$basedir/gdb-port

cxl_backend_file="/tmp/cxltest0.raw"

# NUMA 设备文件放在 1G 大页 hugetlbfs 上（/mnt/huge1G），与 dsm-scripts/config_memdev.sh 一致
numa_memdev_dir="/mnt/huge1G"
# Create 8 CXL device file paths（顺序需与 dsm-scripts/numa_sizes.conf 中 NUMA_SIZES 对应）
numa_devs=(
	"$numa_memdev_dir/numa0.0-$USER"
	"$numa_memdev_dir/numa0.1-$USER"
	"$numa_memdev_dir/numa1.0-$USER"
	"$numa_memdev_dir/numa1.1-$USER"
	"$numa_memdev_dir/numa2.0-$USER"
	"$numa_memdev_dir/numa2.1-$USER"
	"$numa_memdev_dir/numa3.0-$USER"
	"$numa_memdev_dir/numa3.1-$USER"
)

# Build QEMU command with 8 CXL devices
qemu_cmd="@qemu@ -gdb tcp::$port"

# Add 8 CXL devices as ivshmem-plain devices (not NUMA nodes)，size 直接读取对应文件实际大小
for i in {0..7}; do
	dev_path=${numa_devs[$i]}
	if [ -f "$dev_path" ]; then
		per_numa_size_bytes=$(stat -c%s "$dev_path")
	else
		# 兜底：文件不存在时使用默认大小（按 G 转成字节）
		per_numa_size_bytes=$(( ${numa_size%G} * 1024 * 1024 * 1024 ))
	fi

	qemu_cmd="$qemu_cmd -object memory-backend-ram,size=$per_numa_size_bytes,share=on,mem-path=$dev_path,id=cxl$i"
	qemu_cmd="$qemu_cmd -device ivshmem-plain,memdev=cxl$i"
done

# 读取 cxl shared mem 文件和 hostfs 文件的真实大小
if [ -f "$ivshmem_dev" ]; then
	cxl_size_bytes=$(stat -c%s "$ivshmem_dev")
else
	cxl_size_bytes=$(( ${cxl_size%G} * 1024 * 1024 * 1024 ))
fi

if [ -f "$ivshmem_hostfs_dev" ]; then
	hostfs_size_bytes=$(stat -c%s "$ivshmem_hostfs_dev")
else
	hostfs_size_bytes=$(( ${hostfs_size%G} * 1024 * 1024 * 1024 ))
fi

# 使用实际文件大小替换 @qemu_options@ 中 cxl/hostfs 的 size 参数
qemu_options_updated="@qemu_options@"
qemu_options_updated=$(echo "$qemu_options_updated" | sed "s#-object memory-backend-ram,size=[^,]*,share=on,mem-path=$ivshmem_dev#-object memory-backend-file,size=${cxl_size_bytes},share=on,mem-path=$ivshmem_dev#g")
qemu_options_updated=$(echo "$qemu_options_updated" | sed "s#-object memory-backend-file,size=[^,]*,share=on,mem-path=$ivshmem_hostfs_dev#-object memory-backend-file,size=${hostfs_size_bytes},share=on,mem-path=$ivshmem_hostfs_dev#g")

# Add remaining QEMU options (includes ivshmem configuration，已按实际大小修正)
qemu_cmd="$qemu_cmd $qemu_options_updated"

$basedir/../scripts/qemu/qemu_wrapper.sh $vm_id $qemu_cmd | tee exec_log$vm_id.log
# @qemu@ -S -gdb tcp::$port @qemu_options@ | tee exec_log$vm_id.log
