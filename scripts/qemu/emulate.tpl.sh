#!/bin/bash

set -e

basedir=$(dirname "$0")
# basedir should be /build directory
nvm_backend_file="/tmp/nvm-file-$USER"

ivshmem_dev="/dev/shm/ivshmem-$USER"

port=$(shuf -i 30000-40000 -n 1)
#while true; do
#	port=\$(shuf -i 30000-40000 -n 1)
#	netstat -tan | grep \$port > /dev/null 2>&1
#	if [[ \$? -ne 0 ]]; then
#		break
#	fi
#done

echo $port >$basedir/gdb-port

cxl_backend_file="/tmp/cxltest0.raw"

$basedir/../scripts/qemu/qemu_wrapper.sh \
	@qemu@ -gdb tcp::$port @qemu_options@ | tee exec_log
# @qemu@ -S -gdb tcp::$port @qemu_options@ | tee exec_log
