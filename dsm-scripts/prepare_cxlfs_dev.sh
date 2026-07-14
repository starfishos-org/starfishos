#!/bin/bash
# Create (or recreate) the CXLFS ivshmem backing file under /dev/shm.
#
# Usage:
#   ./dsm-scripts/prepare_cxlfs_dev.sh           # idempotent: keep existing file
#   ./dsm-scripts/prepare_cxlfs_dev.sh recreate  # wipe and recreate
# Env:
#   CXLFS_DEV   override device path (default /dev/shm/ivshmem-cxlfs-$USER)
#   CXLFS_FORCE=1  same as "recreate"

set -euo pipefail

device=${CXLFS_DEV:-/dev/shm/ivshmem-cxlfs-$USER}
size=8G
mode="${1:-ensure}"

if [ "${CXLFS_FORCE:-0}" = "1" ]; then
	mode=recreate
fi

case "$mode" in
	ensure|"")
		if [ -e "$device" ]; then
			echo "CXLFS device already exists; leaving it unchanged: $device"
			exit 0
		fi
		;;
	recreate|force|new)
		if [ -e "$device" ]; then
			rm -f "$device"
			echo "Removed existing CXLFS device: $device"
		fi
		;;
	*)
		echo "Usage: $0 [ensure|recreate]" >&2
		exit 1
		;;
esac

umask 077
truncate -s "$size" "$device"
# Little-endian encoding of CXLFS_MAGIC.  CXLFS formatting turns this marker
# into superblock slot 0 while preserving the same first eight bytes.
printf '\x31\x30\x31\x53\x46\x4c\x58\x43' | dd of="$device" bs=8 count=1 conv=notrunc status=none
echo "Created persistent CXLFS ivshmem device: $device ($size)"
