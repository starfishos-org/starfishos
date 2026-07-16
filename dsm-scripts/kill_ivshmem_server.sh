#!/usr/bin/env bash

# Kill this user's ivshmem-server and remove its runtime files.

set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "Usage: $0" >&2
    exit 1
fi
USER=${USER:-$(whoami)}
case "$USER" in
    ""|*[!A-Za-z0-9._-]*)
        echo "Refusing unsafe USER value for ivshmem cleanup: $USER" >&2
        exit 1
        ;;
esac
PID_FILE="/tmp/ivshmem-server-$USER.pid"
SOCKET_PATH="/tmp/ivshmem-doorbell-$USER"
SHM_PATH="/dev/shm/ivshmem-doorbell-$USER"

pid_matches_doorbell() {
    local pid="$1" arg expect_socket=0 process_uid

    [ -L "/proc/$pid/exe" ] || return 1
    process_uid="$(awk '/^Uid:/ { print $2; exit }' "/proc/$pid/status" 2>/dev/null)" \
        || return 1
    [ "$process_uid" = "$(id -u)" ] || return 1
    [ "$(basename "$(readlink -f "/proc/$pid/exe")")" = "ivshmem-server" ] \
        || return 1
    while IFS= read -r -d '' arg; do
        if [ "$expect_socket" -eq 1 ]; then
            [ "$arg" = "$SOCKET_PATH" ]
            return
        fi
        [ "$arg" = "-S" ] && expect_socket=1
    done < "/proc/$pid/cmdline"
    return 1
}

if [ -L "$PID_FILE" ]; then
    echo "Refusing symlink ivshmem PID file: $PID_FILE" >&2
    exit 1
fi
if [ -e "$PID_FILE" ] && { [ ! -f "$PID_FILE" ] \
    || [ "$(stat -c '%u' "$PID_FILE")" != "$(id -u)" ]; }; then
    echo "Refusing unexpected ivshmem PID file: $PID_FILE" >&2
    exit 1
fi
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null || true)
    if [[ "$PID" =~ ^[0-9]+$ ]] && [ -d "/proc/$PID" ]; then
        if ! pid_matches_doorbell "$PID"; then
            echo "Refusing to kill PID $PID: it is not this user's recorded doorbell server." >&2
            exit 1
        fi
        kill "$PID" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$PID" 2>/dev/null; then
            if ! pid_matches_doorbell "$PID"; then
                echo "Refusing to kill reused PID $PID after doorbell shutdown." >&2
                exit 1
            fi
            kill -9 "$PID" 2>/dev/null || true
            sleep 0.1
        fi
        if kill -0 "$PID" 2>/dev/null && pid_matches_doorbell "$PID"; then
            echo "Doorbell server PID $PID could not be stopped." >&2
            exit 1
        fi
    fi
fi

# A missing/stale PID file must not make an active socket or shared-memory
# mapping look disposable.  This is a read-only final identity check; never
# use fuser's kill mode here.
for resource in "$SOCKET_PATH" "$SHM_PATH"; do
    [ -e "$resource" ] || [ -L "$resource" ] || continue
    command -v fuser >/dev/null 2>&1 || {
        echo "Refusing to remove ivshmem runtime without fuser: $resource" >&2
        exit 1
    }
    fuser_output=""
    if fuser_output="$(fuser "$resource" 2>&1)"; then
        echo "Refusing to remove ivshmem runtime still in use: $resource" >&2
        fuser -v "$resource" >&2 || true
        exit 1
    elif [ -n "$fuser_output" ]; then
        echo "Cannot safely inspect ivshmem runtime users for $resource:" >&2
        echo "$fuser_output" >&2
        exit 1
    fi
done
rm -f -- "$PID_FILE" "$SOCKET_PATH" "$SHM_PATH"
