#!/bin/bash

machine_num=$1
machine_id=$2

qemu_memory_args=""

for id in $(seq 1 $machine_num); do
	echo "$id"
	qemu_memory_args="$qemu_memory_args -object memory-backend-file,id=mem$id,size=4G,share=on,mem-path=/dev/shm/shmem$id"
	if [ $id -ne $machine_id ]; then
		qemu_memory_args="$qemu_memory_args -device pc-dimm,id=dimm$id,memdev=mem$id"
	fi
done

echo "$qemu_memory_args"

