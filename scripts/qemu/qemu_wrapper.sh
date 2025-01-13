#!/bin/bash

set -e

# return true if @v1 <= @v2
verlte() {
	[ "$2" = "$(echo -e "$2\n$3" | sort -V | head -n 1)" ]
}

verlt() {
	[ "$2" = "$3" ] && return 1 || verlte $2 $3
}

vm_id=$1
qemu=$2
shift
shift
qemu_options=$@
echo "vm_id: $vm_id, qemu: $qemu, qemu_options: $qemu_options"
qemu_version_str=$($qemu --version | head -n 1)
export IFS=' '
flag="false"
qemu_version=${qemu_version_str}
for str in ${qemu_version_str}; do
	if [[ "${str}" == "version" ]]; then
		flag="true"
	elif [[ ${flag} == "true" ]]; then
		qemu_version=${str}
		break
	fi
done
unset IFS

if [[ "$qemu" == *"qemu-system-aarch64"* ]]; then
	if verlt $qemu_version 6.2.0; then
		# in qemu < 6.2.0, machine type = raspi3
		# in qemu >= 6.2.0, machine type = raspi3b
		qemu_options=$(echo $qemu_options | sed 's/-machine[ \t]\{1,\}raspi3b/-machine raspi3/g')
	fi
fi

numactl -N $vm_id -m $vm_id $qemu $qemu_options
