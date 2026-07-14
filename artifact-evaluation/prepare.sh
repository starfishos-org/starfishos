#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-ensure}"
SERVER_PID_FILE="/tmp/ivshmem-server-$USER.pid"

cd "$REPO_ROOT"

ensure_backing_files() {
    local cxl_file="/dev/shm/ivshmem-$USER"
    local hostfs_file="/dev/shm/ivshmem-hostfs-$USER"
    local numa_files=(
        "/dev/shm/numa0.0-$USER"
        "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER"
        "/dev/shm/numa1.1-$USER"
        "/dev/shm/numa2.0-$USER"
        "/dev/shm/numa2.1-$USER"
        "/dev/shm/numa3.0-$USER"
        "/dev/shm/numa3.1-$USER"
    )
    local missing_numa=0
    local f

    for f in "${numa_files[@]}"; do
        if [ ! -f "$f" ]; then
            missing_numa=1
            break
        fi
    done

    if [ "$missing_numa" = "1" ]; then
        ./dsm-scripts/config_memdev.sh numa-new
    else
        echo "NUMA backing files already exist."
    fi

    if [ ! -f "$cxl_file" ]; then
        ./dsm-scripts/config_memdev.sh cxl-new
    else
        echo "CXL backing file already exists: $cxl_file"
    fi

    if [ ! -f "$hostfs_file" ]; then
        ./dsm-scripts/config_memdev.sh hostfs-new
    else
        echo "hostfs backing file already exists: $hostfs_file"
    fi

    if python3 ./dsm-scripts/prepare_hostfs.py --check; then
        echo "hostfs metadata is current; skipping dataset copy."
    else
        echo "Initializing hostfs metadata and dataset contents."
        python3 ./dsm-scripts/prepare_hostfs.py
    fi
    ./dsm-scripts/prepare_cxlfs_dev.sh
    python3 ./dsm-scripts/prepare_cxlmem.py
}

case "$MODE" in
    ensure)
        echo "=== Ensuring global AE backing files ==="
        ensure_backing_files
        ;;
    recreate)
        echo "=== Recreating global AE backing files ==="
        ./dsm-scripts/config_memdev.sh new-all
        ./dsm-scripts/prepare_cxlfs_dev.sh
        ;;
    *)
        echo "Usage: $0 [ensure|recreate]" >&2
        exit 1
        ;;
esac

echo "=== Starting global AE ivshmem doorbell server ==="
make start-ivshmem-server
sleep 1
if [ ! -f "$SERVER_PID_FILE" ] || ! ps -p "$(cat "$SERVER_PID_FILE")" >/dev/null 2>&1; then
    echo "ivshmem doorbell server did not stay running; see ivshmem_server.log" >&2
    exit 1
fi

echo "=== Resetting DSM metadata ==="
make clean-dsm-meta

echo "Global AE environment is ready."
