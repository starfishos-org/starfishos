#!/usr/bin/env bash
# Remove per-user ivshmem / NUMA / CXLFS backing files under /dev/shm.
# Does not stop the ivshmem doorbell server; use `make kill-ivshmem-server` for that.
set -euo pipefail

mode="remove"
use_sudo_fuser=0
for arg in "$@"; do
    case "$arg" in
        --check) mode="--check" ;;
        --sudo-fuser) use_sudo_fuser=1 ;;
        *)
            echo "Usage: $0 [--check] [--sudo-fuser]" >&2
            exit 1
            ;;
    esac
done

USER=${USER:-$(whoami)}

case "$USER" in
    ""|*[!A-Za-z0-9._-]*)
        echo "Refusing unsafe USER value for memdev cleanup: $USER" >&2
        exit 1
        ;;
esac

files=(
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
)

# Never remove a partially checked set: these files can be mapped by QEMU
# even after it has closed its original file descriptors.  fuser accounts for
# those mappings, while an existence or open-fd-only check does not.
fuser_path="$(command -v fuser 2>/dev/null)" || {
    echo "Cannot safely remove memdev files: required tool fuser is missing." >&2
    exit 1
}
fuser_cmd=("$fuser_path")
if [ "$use_sudo_fuser" -eq 1 ]; then
    command -v sudo >/dev/null 2>&1 || {
        echo "Cannot inspect all memdev users: required tool sudo is missing." >&2
        exit 1
    }
    # Check the exact command first: sudo authentication/policy failure and
    # fuser's normal "no users" result are both nonzero otherwise.
    if ! sudo -n "$fuser_path" -V >/dev/null 2>&1; then
        echo "Cannot inspect all memdev users with non-interactive sudo." >&2
        exit 1
    fi
    fuser_cmd=(sudo -n "$fuser_path")
fi

owner_uid="$(id -u)"
for f in "${files[@]}"; do
    if [ -L "$f" ]; then
        echo "Refusing unexpected memdev path type: $f" >&2
        exit 1
    fi
    [ -e "$f" ] || continue
    if [ ! -f "$f" ]; then
        echo "Refusing unexpected memdev path type: $f" >&2
        exit 1
    fi
    if [ "$(stat -c '%u' "$f")" != "$owner_uid" ]; then
        echo "Refusing memdev file not owned by uid $owner_uid: $f" >&2
        exit 1
    fi
    fuser_output=""
    if fuser_output="$("${fuser_cmd[@]}" "$f" 2>&1)"; then
        echo "Refusing to remove memdev file still in use: $f" >&2
        "${fuser_cmd[@]}" -v "$f" >&2 || true
        exit 1
    elif [ -n "$fuser_output" ]; then
        echo "Cannot safely inspect memdev users for $f:" >&2
        echo "$fuser_output" >&2
        exit 1
    fi
done

if [ "$mode" = "--check" ]; then
    echo "Memdev backing files are safe to remove for user $USER."
    exit 0
fi

removed=0
for f in "${files[@]}"; do
    if [ -e "$f" ]; then
        rm -f -- "$f"
        echo "Removed $f"
        removed=$((removed + 1))
    fi
done

if [ "$removed" -eq 0 ]; then
    echo "No memdev backing files found for user $USER."
else
    echo "Removed $removed memdev backing file(s)."
fi
