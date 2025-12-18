#!/bin/bash

# Kill ivshmem-server

USER=${USER:-$(whoami)}
PID_FILE="/tmp/ivshmem-server-$USER.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    kill "$PID"
fi