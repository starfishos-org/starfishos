#!/usr/bin/env bash
# Host-side QEMU liveness detector.
set -euo pipefail

machine_id=""
qemu_pid=""
event_file=""
interval="0.1"
notify_pid=""
restart_cmd=""
stop_file=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --machine-id) machine_id="$2"; shift 2 ;;
        --qemu-pid) qemu_pid="$2"; shift 2 ;;
        --event-file) event_file="$2"; shift 2 ;;
        --interval) interval="$2"; shift 2 ;;
        --notify-pid) notify_pid="$2"; shift 2 ;;
        --restart-cmd) restart_cmd="$2"; shift 2 ;;
        --stop-file) stop_file="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$machine_id" ] && [ -n "$qemu_pid" ] && [ -n "$event_file" ] || {
    echo "usage: $0 --machine-id ID --qemu-pid PID --event-file FILE [--interval SEC] [--notify-pid PID] [--restart-cmd CMD] [--stop-file FILE]" >&2
    exit 2
}
mkdir -p "$(dirname "$event_file")"
printf 'watching_qemu_pid=%s machine_id=%s started_ns=%s\n' "$qemu_pid" "$machine_id" "$(date +%s%N)" > "$event_file"
while [ ! -e "$stop_file" ] && kill -0 "$qemu_pid" 2>/dev/null; do
    sleep "$interval"
done
if [ -e "$stop_file" ]; then
    printf 'detector_stopped machine_id=%s stopped_ns=%s\n' "$machine_id" "$(date +%s%N)" >> "$event_file"
    exit 0
fi
detected_ns="$(date +%s%N)"
printf 'machine_failure machine_id=%s qemu_pid=%s detected_ns=%s reason=qemu_exit\n' "$machine_id" "$qemu_pid" "$detected_ns" >> "$event_file"
printf 'machine%s_qemu_exited detected_ns=%s\n' "$machine_id" "$detected_ns" >> "$event_file"
if [ -n "$notify_pid" ]; then
    kill -USR1 "$notify_pid" 2>/dev/null || true
fi
if [ -n "$restart_cmd" ]; then
    printf 'machine_restart_requested machine_id=%s requested_ns=%s\n' "$machine_id" "$(date +%s%N)" >> "$event_file"
    # The command must start the replacement QEMU and print its PID.
    new_pid="$(bash -c "$restart_cmd")"
    if [[ "$new_pid" =~ ^[0-9]+$ ]]; then
        printf 'watching_restarted_qemu_pid=%s machine_id=%s\n' "$new_pid" "$machine_id" >> "$event_file"
        exec "$0" --machine-id "$machine_id" --qemu-pid "$new_pid" --event-file "$event_file" --interval "$interval" --notify-pid "$notify_pid" --restart-cmd "$restart_cmd" --stop-file "$stop_file"
    fi
fi
