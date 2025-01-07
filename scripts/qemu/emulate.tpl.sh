#!/bin/bash

set -e

vm_id=0
if [ -n "$1" ]; then
	vm_id=$1
fi
basedir=$(dirname "$0")
# basedir should be /build directory
nvm_backend_file="/tmp/nvm-file-$USER"
ivshmem_dev="/dev/shm/ivshmem-$USER"
dram_size=16G # 16GB shared memory
cxl_size=16G # 16GB shared memory
plat_cpu_name=48 # 48 CPUs
# ivshmem_dev="/dev/dax0.0,align=2M"
# align=2M: refer https://docs.pmem.io/persistent-memory/getting-started-guide/creating-development-environments/virtualization/qemu#nvdimm-io-alignment

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

$basedir/../scripts/qemu/qemu_wrapper.sh \
	@qemu@ -gdb tcp::$port @qemu_options@ | tee exec_log
# @qemu@ -S -gdb tcp::$port @qemu_options@ | tee exec_log
