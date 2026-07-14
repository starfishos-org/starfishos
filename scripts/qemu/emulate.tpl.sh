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
project_root="$basedir/.."
# basedir should be /build directory
nvm_backend_file="/tmp/nvm-file-$USER"
memdev_dir="/dev/shm"
ivshmem_dev="$memdev_dir/ivshmem-$USER"
conn_size=16G
ivshmem_conn_dev="$memdev_dir/ivshmem-conn-$USER"
hostfs_size=16G
ivshmem_hostfs_dev="$memdev_dir/ivshmem-hostfs-$USER"
tmp_size=${TMP_SIZE:-1G}
dram_size=${DRAM_SIZE:-32G}
machine_num=${MACHINE_NUM:-1}
cpu_num=${CPU_NUM:-12}
cxl_size=${CXL_SIZE:-64G}
cxlfs_dev_size=8G
cxlfs_dev=${CXLFS_DEV:-$memdev_dir/ivshmem-cxlfs-$USER}

ini_loader="$project_root/scripts/common/load_chcore_ini.sh"
if [ -f "$ini_loader" ]; then
	# shellcheck source=/dev/null
	. "$ini_loader"
	load_chcore_ini
	[ -n "$CHCORE_INI_MACHINE_NUM" ] && machine_num=${MACHINE_NUM:-$CHCORE_INI_MACHINE_NUM}
	[ -n "$CHCORE_INI_DRAM_SIZE" ] && dram_size=${DRAM_SIZE:-$CHCORE_INI_DRAM_SIZE}
	[ -n "$CHCORE_INI_CPU_NUM" ] && cpu_num=${CPU_NUM:-$CHCORE_INI_CPU_NUM}
	[ -n "$CHCORE_INI_CXL_SIZE" ] && cxl_size=${CXL_SIZE:-$CHCORE_INI_CXL_SIZE}
fi

size_to_bytes() {
	local s="$1"
	local n unit

	n="${s%[KkMmGg]}"
	unit="${s#$n}"

	if [[ "$n" == "$s" ]]; then
		echo "$s"
		return
	fi

	case "$unit" in
		K|k) echo $(( n * 1024 ));;
		M|m) echo $(( n * 1024 * 1024 ));;
		G|g) echo $(( n * 1024 * 1024 * 1024 ));;
		*) echo "0";;
	esac
}

# ivshmem-plain BAR size must be a power of two (QEMU pci_register_bar).
round_up_pow2() {
	local n=$1
	local p=1

	[ "${n:-0}" -lt 1 ] && echo 4096 && return
	while [ "$p" -lt "$n" ]; do
		p=$((p * 2))
	done
	echo "$p"
}

tmp_size_bytes=$(size_to_bytes "$tmp_size")
dram_size_bytes=$(size_to_bytes "$dram_size")
total_mem_size_bytes=$(( tmp_size_bytes + dram_size_bytes ))
gib=$(( 1024 * 1024 * 1024 ))
total_mem_size_gib=$(( (total_mem_size_bytes + gib - 1) / gib ))
total_mem_size_qemu="${total_mem_size_gib}G"
cxlfs_dev_size_bytes=$(size_to_bytes "$cxlfs_dev_size")
numa_size=16G # 默认大小，仅在对应文件不存在时兜底
plat_cpu_name=$cpu_num
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

cxl_backend_file="/dev/shm/ivshmem-$USER"

# NUMA 设备文件改放在 /dev/shm，保持与 dsm-scripts/config_memdev.sh 一致
numa_memdev_dir="/dev/shm"
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

# Build QEMU command; optional 8x NUMA ivshmem-plain for guest local DRAM
qemu_cmd="@qemu@ -gdb tcp::$port"

# 与 CMake kernel USE_DEV_AS_DRAM 一致（@qemu_use_dev_as_dram@）；未 export 时按编译配置默认开启/关闭。
# 仍可显式设置 USE_DEV_AS_DRAM=0 跳过 NUMA 设备（仅当内核未使用 USE_DEV_AS_DRAM 时才有意义）。
: "${USE_DEV_AS_DRAM:=@qemu_use_dev_as_dram@}"
if [ "${USE_DEV_AS_DRAM}" = "1" ]; then
	for i in {0..7}; do
		dev_path=${numa_devs[$i]}
		if [ ! -f "$dev_path" ]; then
			echo "[FATAL] USE_DEV_AS_DRAM=1 需要 8 个 NUMA backing 文件，缺失: $dev_path" >&2
			echo "请先运行: dsm-scripts/config_memdev.sh new_numa" >&2
			exit 1
		fi
		per_numa_size_bytes=$(stat -c%s "$dev_path")
		if command -v truncate >/dev/null 2>&1; then
			np=$(round_up_pow2 "$per_numa_size_bytes")
			if [ "$np" -ne "$per_numa_size_bytes" ]; then
				truncate -s "$np" "$dev_path"
			fi
			per_numa_size_bytes=$np
		fi

		qemu_cmd="$qemu_cmd -object memory-backend-file,size=$per_numa_size_bytes,share=on,mem-path=$dev_path,id=cxl$i"
		qemu_cmd="$qemu_cmd -device ivshmem-plain,memdev=cxl$i"
	done
fi

# 读取 cxl shared mem 文件和 hostfs 文件的真实大小（须与 QEMU size= 一致）
if [ -f "$ivshmem_dev" ]; then
	cxl_size_bytes=$(stat -c%s "$ivshmem_dev")
	if [ "${cxl_size_bytes:-0}" -eq 0 ] && command -v truncate >/dev/null 2>&1; then
		truncate -s "$cxl_size" "$ivshmem_dev"
		cxl_size_bytes=$(stat -c%s "$ivshmem_dev")
	fi
	if [ "${cxl_size_bytes:-0}" -eq 0 ]; then
		cxl_size_bytes=$(( ${cxl_size%G} * 1024 * 1024 * 1024 ))
	fi
else
	cxl_size_bytes=$(( ${cxl_size%G} * 1024 * 1024 * 1024 ))
fi

if [ -f "$ivshmem_hostfs_dev" ]; then
	hostfs_size_bytes=$(stat -c%s "$ivshmem_hostfs_dev")
	if [ "${hostfs_size_bytes:-0}" -eq 0 ] && command -v truncate >/dev/null 2>&1; then
		truncate -s "$hostfs_size" "$ivshmem_hostfs_dev"
		hostfs_size_bytes=$(stat -c%s "$ivshmem_hostfs_dev")
	fi
	if [ "${hostfs_size_bytes:-0}" -eq 0 ]; then
		hostfs_size_bytes=$(( ${hostfs_size%G} * 1024 * 1024 * 1024 ))
	fi
else
	hostfs_size_bytes=$(( ${hostfs_size%G} * 1024 * 1024 * 1024 ))
fi

# CXLFS has its own CXL-like ivshmem device.  VM startup never creates,
# truncates, or clears it: its bytes are the authoritative filesystem image.
if [ ! -f "$cxlfs_dev" ]; then
	echo "[FATAL] CXLFS ivshmem device 不存在: $cxlfs_dev" >&2
	echo "请先运行: ./dsm-scripts/prepare_cxlfs_dev.sh" >&2
	exit 1
fi
cxlfs_dev_file_size=$(stat -c%s "$cxlfs_dev")
if [ "$cxlfs_dev_file_size" -ne "$cxlfs_dev_size_bytes" ]; then
	echo "[FATAL] CXLFS ivshmem device 尺寸错误: $cxlfs_dev ($cxlfs_dev_file_size, expected $cxlfs_dev_size_bytes)" >&2
	exit 1
fi

if [ -f "$ivshmem_dev" ] && command -v truncate >/dev/null 2>&1; then
	np=$(round_up_pow2 "$cxl_size_bytes")
	if [ "$np" -ne "$cxl_size_bytes" ]; then
		truncate -s "$np" "$ivshmem_dev"
	fi
	cxl_size_bytes=$np
fi

if [ -f "$ivshmem_hostfs_dev" ] && command -v truncate >/dev/null 2>&1; then
	np=$(round_up_pow2 "$hostfs_size_bytes")
	if [ "$np" -ne "$hostfs_size_bytes" ]; then
		truncate -s "$np" "$ivshmem_hostfs_dev"
	fi
	hostfs_size_bytes=$np
fi

# DSM IVSHMEM（CMake 在 qemu_options 里包含 ivshmem-plain 时置 @qemu_emulate_ivshmem_plain@=1）：
# 直接用 stat/truncate 对齐后的字节数拼接参数，不再对占位串做 sed。
if [ "@qemu_emulate_ivshmem_plain@" = "1" ]; then
	qemu_options_updated="--enable-kvm -M q35 -name chcore-$vm_id -m $total_mem_size_qemu \
-object memory-backend-file,size=${cxl_size_bytes},share=on,mem-path=$ivshmem_dev,id=hostmem \
-device ivshmem-plain,memdev=hostmem \
-object memory-backend-file,size=${hostfs_size_bytes},share=on,mem-path=$ivshmem_hostfs_dev,id=hostfsmem \
-device ivshmem-plain,memdev=hostfsmem \
-object memory-backend-file,size=${cxlfs_dev_size_bytes},share=on,mem-path=$cxlfs_dev,id=cxlfsmem \
-device ivshmem-plain,memdev=cxlfsmem \
-chardev socket,path=/tmp/ivshmem-doorbell-$USER,id=doorbell_chardev \
-device ivshmem-doorbell,chardev=doorbell_chardev,vectors=16 \
-cpu host -smp $plat_cpu_name -serial mon:stdio -nographic \
-cdrom $basedir/chcore.iso \
-fw_cfg name=opt/chcore/bootargs,string=machine_id=$vm_id,,machine_num=$machine_num,,cpu_num=$cpu_num,,tmp_size=$tmp_size,,dram_size=$dram_size"
else
	qemu_options_updated="@qemu_options@"
	# 非 IVSHMEM 布局仍使用 CMake 生成的 @qemu_options@（SLS/CXL 等）。
	qemu_options_updated=$(echo "$qemu_options_updated" | sed "s#-m [^ ]\\+#-m $total_mem_size_qemu#g")
	qemu_options_updated=$(echo "$qemu_options_updated" | sed "s#\\(name=opt/chcore/bootargs,string=machine_id=$vm_id\\)#\\1,,machine_num=$machine_num,,cpu_num=$cpu_num,,tmp_size=$tmp_size,,dram_size=$dram_size#g")
fi

# Add remaining QEMU options (includes ivshmem configuration，已按实际大小修正)
qemu_cmd="$qemu_cmd $qemu_options_updated"

log_dir="$project_root/logs"
mkdir -p "$log_dir"
$basedir/../scripts/qemu/qemu_wrapper.sh $vm_id $qemu_cmd | tee "$log_dir/exec_log$vm_id.log"
# @qemu@ -S -gdb tcp::$port @qemu_options@ | tee exec_log$vm_id.log
