#!/bin/bash

set -e

device=${CXLFS_DEV:-/dev/shm/ivshmem-cxlfs-$USER}
size=8G

if [ -e "$device" ]; then
	echo "CXLFS device already exists; leaving it unchanged: $device"
	exit 0
fi

umask 077
truncate -s "$size" "$device"
# Little-endian encoding of CXLFS_MAGIC.  CXLFS formatting turns this marker
# into superblock slot 0 while preserving the same first eight bytes.
printf '\x31\x30\x31\x53\x46\x4c\x58\x43' | dd of="$device" bs=8 count=1 conv=notrunc status=none
echo "Created persistent CXLFS ivshmem device: $device ($size)"
