#!/bin/bash

# Start ivshmem-server for doorbell communication
# This script should be run before starting any QEMU instances

set -e

USER=${USER:-$(whoami)}
SOCKET_PATH="/tmp/ivshmem-doorbell-$USER"
PID_FILE="/tmp/ivshmem-server-$USER.pid"
SHM_NAME="ivshmem-doorbell-$USER"
SHM_SIZE="1M"  # Small size for doorbell only (data is in ivshmem-plain)
VECTORS=16     # Number of interrupt vectors

# Function to cleanup old resources
cleanup_old_resources() {
    echo "Cleaning up old resources..."
    
    # Kill old ivshmem-server process if running
    if [ -f "$PID_FILE" ]; then
        OLD_PID=$(cat "$PID_FILE" 2>/dev/null || echo "")
        if [ -n "$OLD_PID" ] && ps -p "$OLD_PID" > /dev/null 2>&1; then
            echo "  Killing old ivshmem-server process (PID: $OLD_PID)"
            kill "$OLD_PID" 2>/dev/null || true
            sleep 1
            # Force kill if still running
            if ps -p "$OLD_PID" > /dev/null 2>&1; then
                kill -9 "$OLD_PID" 2>/dev/null || true
            fi
        fi
        rm -f "$PID_FILE"
    fi
    
    # Also check for any other ivshmem-server processes using this socket
    if [ -e "$SOCKET_PATH" ]; then
        echo "  Removing old socket: $SOCKET_PATH"
        rm -f "$SOCKET_PATH"
    fi
    
    # Clean up shared memory if exists
    if [ -e "/dev/shm/$SHM_NAME" ]; then
        echo "  Removing old shared memory: /dev/shm/$SHM_NAME"
        rm -f "/dev/shm/$SHM_NAME"
    fi
    
    # Wait a bit for cleanup to complete
    sleep 0.5
}

# Find ivshmem-server binary
SCRIPT_DIR=$(dirname "$0")
PROJECT_ROOT="$SCRIPT_DIR/.."
IVSHMEM_SERVER=""

# Check common locations (including the path from terminal output)
if [ -f "/disk/wfn/qemu-6.2.0/build/contrib/ivshmem-server/ivshmem-server" ]; then
    IVSHMEM_SERVER="/disk/wfn/qemu-6.2.0/build/contrib/ivshmem-server/ivshmem-server"
elif [ -f "$PROJECT_ROOT/qemu-6.2.0-ivshmem/build/ivshmem-server" ]; then
    IVSHMEM_SERVER="$PROJECT_ROOT/qemu-6.2.0-ivshmem/build/ivshmem-server"
elif [ -f "$PROJECT_ROOT/qemu-6.2.0-ivshmem/ivshmem-server/ivshmem-server" ]; then
    IVSHMEM_SERVER="$PROJECT_ROOT/qemu-6.2.0-ivshmem/ivshmem-server/ivshmem-server"
elif command -v ivshmem-server > /dev/null 2>&1; then
    IVSHMEM_SERVER="ivshmem-server"
else
    echo "Error: ivshmem-server not found"
    echo "Please build it first: ./scripts/build_ivshmem_server.sh"
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
echo ""

$IVSHMEM_SERVER -F -v \
    -S "$SOCKET_PATH" \
    -M "$SHM_NAME" \
    -l "$SHM_SIZE" \
    -n "$VECTORS" \
    > ivshmem_server.log 2>&1 &

job_pid=$!
server_pid=$(jobs -p | head -n1)

echo $server_pid

echo $server_pid > $PID_FILE

echo "ivshmem-server started successfully (PID: $(cat $PID_FILE) in ivshmem_server.log)"
