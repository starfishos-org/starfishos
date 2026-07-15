#!/bin/bash
# Create (or recreate) the CXLFS ivshmem backing file under /dev/shm.
#
# Usage:
#   ./dsm-scripts/prepare_cxlfs_dev.sh           # keep only if current build matches
#   ./dsm-scripts/prepare_cxlfs_dev.sh recreate  # wipe and recreate
# Env:
#   CXLFS_DEV   override device path (default /dev/shm/ivshmem-cxlfs-$USER)
#   CXLFS_FORCE=1  same as "recreate"
#   CXLFS_BUILD_IMAGE  build artifact used to detect a stale persistent image
#   CXLFS_BUILD_STAMP  override build-ID sidecar (default: $CXLFS_DEV.build-id)

set -euo pipefail

device=${CXLFS_DEV:-/dev/shm/ivshmem-cxlfs-$USER}
stamp=${CXLFS_BUILD_STAMP:-${device}.build-id}
size=8G
mode="${1:-ensure}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_image=${CXLFS_BUILD_IMAGE:-$repo_root/user/build/ramdisk.cpio}

# CXLFS persists a copy of the boot ramdisk. Reusing the same per-user
# /dev/shm file after switching clones or rebuilding can make that copy
# incompatible with the current embedded image (for example, a different
# libc.so size). Path + inode + size + nanosecond mtime is cheap to compute
# and changes whenever the ramdisk archive is replaced or rebuilt.
build_id=""
if [ -f "$build_image" ]; then
	build_id="$({
		readlink -f "$build_image"
		stat -Lc '%d:%i:%s:%y' "$build_image"
	} | sha256sum | awk '{print $1}')"
fi

if [ "${CXLFS_FORCE:-0}" = "1" ]; then
	mode=recreate
fi

case "$mode" in
	ensure|"")
		if [ -e "$device" ]; then
			old_build_id="$(cat "$stamp" 2>/dev/null || true)"
			if [ -z "$build_id" ]; then
				echo "CXLFS device already exists; build image is unavailable, leaving it unchanged: $device"
				exit 0
			fi
			if [ "$old_build_id" = "$build_id" ]; then
				echo "CXLFS device matches the current ramdisk build: $device"
				exit 0
			fi
			echo "CXLFS device belongs to a different or unknown ramdisk build; recreating it."
			mode=recreate
		fi
		;;
	recreate|force|new)
		;;
	*)
		echo "Usage: $0 [ensure|recreate]" >&2
		exit 1
		;;
esac

if [ "$mode" = "recreate" ] || [ "$mode" = "force" ] || [ "$mode" = "new" ]; then
	if [ -e "$device" ]; then
		rm -f "$device"
		echo "Removed existing CXLFS device: $device"
	fi
	rm -f "$stamp"
fi

umask 077
truncate -s "$size" "$device"
# Little-endian encoding of CXLFS_MAGIC.  CXLFS formatting turns this marker
# into superblock slot 0 while preserving the same first eight bytes.
printf '\x31\x30\x31\x53\x46\x4c\x58\x43' | dd of="$device" bs=8 count=1 conv=notrunc status=none
if [ -n "$build_id" ]; then
	printf '%s\n' "$build_id" > "$stamp"
else
	rm -f "$stamp"
	echo "Warning: $build_image is not built yet; the device will be refreshed before the first AE boot." >&2
fi
echo "Created persistent CXLFS ivshmem device: $device ($size)"
