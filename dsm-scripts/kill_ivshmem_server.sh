#!/usr/bin/env bash

# Kill this user's ivshmem-server and remove its runtime files.

set -euo pipefail

USER=${USER:-$(whoami)}
PID_FILE="/tmp/ivshmem-server-$USER.pid"
SOCKET_PATH="/tmp/ivshmem-doorbell-$USER"
SHM_PATH="/dev/shm/ivshmem-doorbell-$USER"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null || true)
    if [ -n "$PID" ] && [ -L "/proc/$PID/exe" ] \
        && [ "$(basename "$(readlink -f "/proc/$PID/exe")")" = "ivshmem-server" ]; then
        kill "$PID" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$PID" 2>/dev/null; then
            kill -9 "$PID" 2>/dev/null || true
        fi
    fi
fi
rm -f "$PID_FILE" "$SOCKET_PATH" "$SHM_PATH"
