#!/usr/bin/env bash

# Reserve a QEMU GDB TCP port across concurrently starting emulate.sh
# processes.  The lock file descriptor intentionally remains open in the
# caller until QEMU exits, closing the check-to-bind race between wrappers.

chcore_gdb_port_is_free() {
	local port="$1"

	if command -v ss >/dev/null 2>&1; then
		! ss -H -ltn "sport = :$port" 2>/dev/null | grep -q .
	elif command -v netstat >/dev/null 2>&1; then
		! netstat -ltn 2>/dev/null | awk -v port="$port" '
			NR > 2 && $4 ~ (":" port "$") { found = 1 }
			END { exit !found }
		'
	else
		# The inter-wrapper lock still prevents the collision seen in AE-023.
		# QEMU will report the uncommon collision with an unrelated process.
		return 0
	fi
}

chcore_reserve_gdb_port() {
	local min_port="${CHCORE_GDB_PORT_MIN:-30000}"
	local max_port="${CHCORE_GDB_PORT_MAX:-40000}"
	local lock_dir="${CHCORE_GDB_LOCK_DIR:-${XDG_RUNTIME_DIR:-/tmp}/chcore-gdb-$USER}"
	local attempts="${CHCORE_GDB_PORT_ATTEMPTS:-256}"
	local attempt candidate lock_fd

	command -v flock >/dev/null 2>&1 || {
		echo "[FATAL] flock is required to reserve a QEMU GDB port" >&2
		return 1
	}
	[[ "$min_port" =~ ^[0-9]+$ && "$max_port" =~ ^[0-9]+$ ]] || {
		echo "[FATAL] invalid QEMU GDB port range: $min_port-$max_port" >&2
		return 1
	}
	if [ "$min_port" -lt 1 ] || [ "$max_port" -gt 65535 ] || [ "$min_port" -gt "$max_port" ]; then
		echo "[FATAL] invalid QEMU GDB port range: $min_port-$max_port" >&2
		return 1
	fi

	mkdir -p "$lock_dir"
	chmod 700 "$lock_dir"
	for attempt in $(seq 1 "$attempts"); do
		candidate=$(shuf -i "$min_port-$max_port" -n 1)
		exec {lock_fd}>"$lock_dir/port-$candidate.lock"
		if flock -n "$lock_fd" && chcore_gdb_port_is_free "$candidate"; then
			CHCORE_GDB_PORT="$candidate"
			CHCORE_GDB_LOCK_FD="$lock_fd"
			return 0
		fi
		exec {lock_fd}>&-
	done

	echo "[FATAL] no free QEMU GDB port in $min_port-$max_port after $attempts attempts" >&2
	return 1
}
