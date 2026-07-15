#!/usr/bin/env bash
# Install host-side dependencies for the Starfish artifact on Ubuntu/Debian.
set -euo pipefail

if [ "${EUID}" -eq 0 ]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || {
        echo "sudo is required when this script is not run as root." >&2
        exit 1
    }
    SUDO=sudo
fi

if [ ! -r /etc/os-release ]; then
    echo "This installer supports Ubuntu/Debian hosts only." >&2
    exit 1
fi

. /etc/os-release
case "${ID:-}" in
    ubuntu|debian) ;;
    *)
        echo "Unsupported distribution: ${PRETTY_NAME:-unknown}. Install the listed packages manually." >&2
        exit 1
        ;;
esac

APT_LOCK_TIMEOUT="${APT_LOCK_TIMEOUT:-60}"

apt_get() {
    $SUDO apt-get -o "DPkg::Lock::Timeout=$APT_LOCK_TIMEOUT" "$@"
}

apt_failure_help() {
    echo >&2
    echo "apt failed. Another apt/dpkg process may hold a package-manager lock." >&2
    echo "Inspect it with: ps -ef | grep -E '[a]pt|[d]pkg'" >&2
    echo "If the owner is stale, ask an administrator to stop it and repair dpkg." >&2
    echo "Set APT_LOCK_TIMEOUT=<seconds> to wait longer, or SKIP_APT=1 only when" >&2
    echo "all host packages listed in this script are already installed." >&2
}

if [ "${SKIP_APT:-0}" != "1" ]; then
    if ! apt_get update; then
        apt_failure_help
        exit 1
    fi
    if ! apt_get install -y \
        ca-certificates \
        build-essential \
        cmake \
        curl \
        docker.io \
        git \
        libglib2.0-dev \
        libnuma-dev \
        libpixman-1-dev \
        libslirp-dev \
        make \
        ninja-build \
        numactl \
        openmpi-bin \
        libopenmpi-dev \
        pkg-config \
        python3 \
        python3-matplotlib \
        python3-numpy \
        python3-pandas \
        python3-pip \
        tmux \
        xz-utils \
        zlib1g-dev
    then
        apt_failure_help
        exit 1
    fi
else
    echo "SKIP_APT=1: skipping apt update/install; assuming host packages exist."
fi

if command -v systemctl >/dev/null 2>&1; then
    $SUDO systemctl enable --now docker
fi

need_relogin=0
if [ "${EUID}" -ne 0 ] && ! id -nG "$USER" | tr ' ' '\n' | grep -qx docker; then
    $SUDO usermod -aG docker "$USER"
    echo "Added $USER to the docker group."
    need_relogin=1
fi

if [ -e /dev/kvm ]; then
    if getent group kvm >/dev/null 2>&1; then
        if [ "${EUID}" -ne 0 ] && ! id -nG "$USER" | tr ' ' '\n' | grep -qx kvm; then
            $SUDO usermod -aG kvm "$USER"
            echo "Added $USER to the kvm group (required for /dev/kvm)."
            need_relogin=1
        fi
    fi
else
    echo "WARNING: /dev/kvm is absent; enable hardware virtualization and KVM before running the artifact." >&2
fi

if [ "$need_relogin" -eq 1 ]; then
    echo "Log out and back in (or re-login via ssh) before running Docker/QEMU without sudo."
fi

echo "Installed Docker, OpenMPI, numactl, tmux, Python 3, matplotlib/numpy/pandas, and QEMU build dependencies."

QEMU_VERSION=6.2.0
QEMU_PREFIX=/usr/local/qemu-6.2
QEMU_BIN=/usr/local/bin/qemu-6.2-system-x86_64
IVSHMEM_SERVER="$QEMU_PREFIX/bin/ivshmem-server"

qemu_ok=0
server_ok=0
if [ -x "$QEMU_BIN" ] && "$QEMU_BIN" --version | grep -q "version $QEMU_VERSION"; then
    qemu_ok=1
    echo "QEMU $QEMU_VERSION is already installed: $QEMU_BIN"
fi
if [ -x "$IVSHMEM_SERVER" ]; then
    server_ok=1
    echo "ivshmem-server is already installed: $IVSHMEM_SERVER"
fi

if [ "$qemu_ok" -ne 1 ] || [ "$server_ok" -ne 1 ]; then
    build_dir=$(mktemp -d)
    trap 'rm -rf "$build_dir"' EXIT
    if [ "$qemu_ok" -eq 1 ]; then
        echo "QEMU exists but ivshmem-server is missing; building the matching QEMU $QEMU_VERSION utility."
    else
        echo "Downloading and building QEMU $QEMU_VERSION from download.qemu.org."
    fi
    curl --fail --location --retry 3 --output "$build_dir/qemu-$QEMU_VERSION.tar.xz" \
        "https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"
    tar -C "$build_dir" -xf "$build_dir/qemu-$QEMU_VERSION.tar.xz"
    cd "$build_dir/qemu-$QEMU_VERSION"
    ./configure --prefix="$QEMU_PREFIX" --target-list=x86_64-softmmu --enable-kvm --disable-werror
    make -j"$(nproc)"
    if [ "$qemu_ok" -ne 1 ]; then
        $SUDO make install
    fi
    $SUDO install -d "$QEMU_PREFIX/bin"
    $SUDO install -m 0755 build/contrib/ivshmem-server/ivshmem-server "$QEMU_PREFIX/bin/ivshmem-server"
    if [ "$qemu_ok" -ne 1 ]; then
        $SUDO ln -sfn "$QEMU_PREFIX/bin/qemu-system-x86_64" "$QEMU_BIN"
    fi
fi

if ! "$QEMU_BIN" --version | grep -q "version $QEMU_VERSION"; then
    echo "QEMU $QEMU_VERSION installation verification failed." >&2
    exit 1
fi
if [ ! -x "$IVSHMEM_SERVER" ]; then
    echo "ivshmem-server installation verification failed: $IVSHMEM_SERVER" >&2
    exit 1
fi

echo "Installed QEMU $QEMU_VERSION as $QEMU_BIN and ivshmem-server as $IVSHMEM_SERVER."
