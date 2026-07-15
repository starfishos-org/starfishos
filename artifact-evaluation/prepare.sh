#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-ensure}"
SERVER_PID_FILE="/tmp/ivshmem-server-$USER.pid"

cd "$REPO_ROOT"

# Required demo / library submodules for the ready AE path. Initialize these
# paths independently so a broken optional test-on-linux submodule cannot
# abort before the required ChCore demos are populated.
ensure_required_submodules() {
    local path missing=0
    local required=(
        "user/libraries/rpmalloc"
        "user/demos/phoenix-2.0"
        "user/demos/dbx1000"
        "user/demos/GeminiGraph"
    )

    echo "=== Ensuring required git submodules ==="
    for path in "${required[@]}"; do
        if [ ! -e "$path/CMakeLists.txt" ]; then
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then
        echo "Required submodule content is missing; initializing only ready-AE paths."
        if ! git submodule update --init --recursive -- "${required[@]}"; then
            echo >&2
            echo "Targeted submodule initialization failed." >&2
        fi
    fi

    missing=0
    for path in "${required[@]}"; do
        if [ ! -e "$path/CMakeLists.txt" ]; then
            echo "Missing submodule content: $path" >&2
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then
        echo >&2
        echo "Retry from the repository root:" >&2
        echo "  git submodule update --init --recursive -- \\" >&2
        echo "    user/libraries/rpmalloc user/demos/phoenix-2.0 \\" >&2
        echo "    user/demos/dbx1000 user/demos/GeminiGraph" >&2
        echo "If one path has a partial checkout, deinitialize and remove only" >&2
        echo "that path and its .git/modules entry before retrying." >&2
        echo >&2
        echo "Note: demo URLs are under https://github.com/starfishos-org/." >&2
        echo "A network fetch is required before the first build can succeed." >&2
        exit 1
    fi
}

# Phoenix data files live under datasets/ (gitignored). download_datasets.sh
# also hardlinks/copies them into user/demos/phoenix-2.0/data/… (and
# test-on-linux/phoenix when that submodule is present) so phoenix-copy-data
# can rsync real files into the ramdisk. twitter-2010.bin (~11 GiB) is only
# needed for Gemini.
download_datasets() {
    echo "=== Downloading benchmark datasets (if missing) ==="
    # Default: fetch twitter-2010.bin (~11 GiB) so auto-scale / Gemini one-click
    # runs have /host/twitter-2010.bin after prepare_hostfs. Set
    # SKIP_GRAPH_DATASET=1 to skip the large graph on hosts that do not need it.
    SKIP_GRAPH_DATASET="${SKIP_GRAPH_DATASET:-0}" \
        ./scripts/download_datasets.sh
}

warn_host_layout() {
    if [ ! -d /sys/devices/system/node/node4 ]; then
        echo "WARNING: NUMA node 4 not found." >&2
        echo "  config_memdev.sh binds the CXL ivshmem file with" >&2
        echo "  numactl --membind=4; allocation may fail on this host." >&2
        echo "  Edit memNumaNode in dsm-scripts/config_memdev.sh if needed." >&2
    fi

    # Rough capacity check: 8x16G NUMA + 64G CXL + 16G hostfs + 8G CXLFS ≈ 216G.
    # Do not warn based on free space when this user's complete backing-file
    # set already exists: ensure mode will reuse it without allocating 216G.
    if [ -d /dev/shm ]; then
        local avail_kb f all_exist=1
        for f in \
            "/dev/shm/ivshmem-$USER" \
            "/dev/shm/ivshmem-hostfs-$USER" \
            "/dev/shm/ivshmem-cxlfs-$USER" \
            "/dev/shm/numa0.0-$USER" "/dev/shm/numa0.1-$USER" \
            "/dev/shm/numa1.0-$USER" "/dev/shm/numa1.1-$USER" \
            "/dev/shm/numa2.0-$USER" "/dev/shm/numa2.1-$USER" \
            "/dev/shm/numa3.0-$USER" "/dev/shm/numa3.1-$USER"
        do
            [ -f "$f" ] || all_exist=0
        done
        if [ "$all_exist" -eq 1 ]; then
            echo "Existing per-user /dev/shm backing files will be reused."
            return
        fi
        avail_kb="$(df -k /dev/shm 2>/dev/null | awk 'NR==2 {print $4}')"
        if [ -n "${avail_kb:-}" ] && [ "$avail_kb" -lt $((200 * 1024 * 1024)) ]; then
            echo "WARNING: /dev/shm has less than ~200 GiB free" \
                "($((avail_kb / 1024 / 1024)) GiB reported)." >&2
            echo "  Default AE backing files need about 216 GiB under /dev/shm." >&2
        fi
    fi
}

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

ensure_required_submodules
download_datasets
warn_host_layout

case "$MODE" in
    ensure)
        echo "=== Ensuring global AE backing files ==="
        ensure_backing_files
        ;;
    recreate)
        echo "=== Recreating global AE backing files ==="
        ./dsm-scripts/config_memdev.sh new-all
        # Force CXLFS rebuild: prepare_cxlfs_dev.sh ensure would skip an
        # existing /dev/shm/ivshmem-cxlfs-$USER left over from a prior run.
        ./dsm-scripts/prepare_cxlfs_dev.sh recreate
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
    echo "ivshmem doorbell server did not stay running; see log/ivshmem-server/server.log" >&2
    exit 1
fi

echo "=== Resetting DSM metadata ==="
make clean-dsm-meta

echo "Global AE environment is ready."
