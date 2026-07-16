#!/usr/bin/env bash

# Start ivshmem-server for doorbell communication
# This script should be run before starting any QEMU instances

set -euo pipefail

USER=${USER:-$(whoami)}
SOCKET_PATH="/tmp/ivshmem-doorbell-$USER"
PID_FILE="/tmp/ivshmem-server-$USER.pid"
SHM_NAME="ivshmem-doorbell-$USER"
SHM_SIZE="1M"  # Small size for doorbell only (data is in ivshmem-plain)
VECTORS=16     # Number of interrupt vectors

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$PROJECT_ROOT/log/ivshmem-server"
LOG_FILE="$LOG_DIR/server.log"

# Function to cleanup old resources
cleanup_old_resources() {
    echo "Cleaning up old resources..."
    "$SCRIPT_DIR/kill_ivshmem_server.sh"
    sleep 0.5
}

# Find ivshmem-server binary. IVSHMEM_SERVER_BIN is useful for non-system
# installs; do not fall back to another developer's home directory.
IVSHMEM_SERVER=""

if [ -n "${IVSHMEM_SERVER_BIN:-}" ] && [ -x "$IVSHMEM_SERVER_BIN" ]; then
    IVSHMEM_SERVER="$IVSHMEM_SERVER_BIN"
elif [ -x "/usr/local/qemu-6.2/bin/ivshmem-server" ]; then
    IVSHMEM_SERVER="/usr/local/qemu-6.2/bin/ivshmem-server"
elif [ -x "$PROJECT_ROOT/qemu-6.2.0-ivshmem/build/ivshmem-server" ]; then
    IVSHMEM_SERVER="$PROJECT_ROOT/qemu-6.2.0-ivshmem/build/ivshmem-server"
elif [ -x "$PROJECT_ROOT/qemu-6.2.0-ivshmem/ivshmem-server/ivshmem-server" ]; then
    IVSHMEM_SERVER="$PROJECT_ROOT/qemu-6.2.0-ivshmem/ivshmem-server/ivshmem-server"
elif command -v ivshmem-server > /dev/null 2>&1; then
    IVSHMEM_SERVER="ivshmem-server"
else
    echo "Error: ivshmem-server not found" >&2
    echo "Run artifact-evaluation/install-host-deps.sh, or set IVSHMEM_SERVER_BIN." >&2
    exit 1
fi

# Cleanup before starting
cleanup_old_resources

# Start ivshmem-server
echo "Starting ivshmem-server..."
echo "  Socket: $SOCKET_PATH"
echo "  Shared memory: $SHM_NAME ($SHM_SIZE)"
echo "  Vectors: $VECTORS"
echo "  PID file: $PID_FILE"
echo "  Log: $LOG_FILE"
echo ""

mkdir -p "$LOG_DIR"
server_pid="$(python3 "$SCRIPT_DIR/spawn_detached.py" \
    --log "$LOG_FILE" -- \
    "$IVSHMEM_SERVER" -F -v \
    -S "$SOCKET_PATH" \
    -M "$SHM_NAME" \
    -l "$SHM_SIZE" \
    -n "$VECTORS")"
[[ "$server_pid" =~ ^[0-9]+$ ]] || {
    echo "Failed to obtain ivshmem-server PID: $server_pid" >&2
    exit 1
}
echo "$server_pid" > "$PID_FILE"

for _ in $(seq 1 50); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "ivshmem-server exited during startup; see $LOG_FILE" >&2
        tail -20 "$LOG_FILE" >&2 || true
        rm -f "$PID_FILE"
        exit 1
    fi
    [ -S "$SOCKET_PATH" ] && break
    sleep 0.1
done
if [ ! -S "$SOCKET_PATH" ]; then
    echo "ivshmem-server did not create $SOCKET_PATH; see $LOG_FILE" >&2
    if ! "$SCRIPT_DIR/kill_ivshmem_server.sh"; then
        echo "Scoped ivshmem-server cleanup failed; runtime files were preserved." >&2
    fi
    exit 1
fi

echo "ivshmem-server started successfully (PID: $server_pid)"
