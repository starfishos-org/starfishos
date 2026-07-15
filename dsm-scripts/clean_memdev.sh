#!/usr/bin/env bash
# Remove per-user ivshmem / NUMA / CXLFS backing files under /dev/shm.
# Does not stop the ivshmem doorbell server; use `make kill-ivshmem-server` for that.
set -euo pipefail

removed=0
for f in \
    "/dev/shm/ivshmem-$USER" \
    "/dev/shm/ivshmem-hostfs-$USER" \
    "/dev/shm/ivshmem-cxlfs-$USER" \
    "/dev/shm/ivshmem-cxlfs-$USER.build-id" \
    "/dev/shm/numa0.0-$USER" \
    "/dev/shm/numa0.1-$USER" \
    "/dev/shm/numa1.0-$USER" \
    "/dev/shm/numa1.1-$USER" \
    "/dev/shm/numa2.0-$USER" \
    "/dev/shm/numa2.1-$USER" \
    "/dev/shm/numa3.0-$USER" \
    "/dev/shm/numa3.1-$USER"
do
    if [ -e "$f" ]; then
        rm -f "$f"
        echo "Removed $f"
        removed=$((removed + 1))
    fi
done

if [ "$removed" -eq 0 ]; then
    echo "No memdev backing files found for user $USER."
else
    echo "Removed $removed memdev backing file(s)."
fi
