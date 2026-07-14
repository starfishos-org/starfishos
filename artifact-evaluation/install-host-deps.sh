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

$SUDO apt-get update
$SUDO apt-get install -y \
    ca-certificates \
    build-essential \
    curl \
    docker.io \
    git \
    libglib2.0-dev \
    libpixman-1-dev \
    libslirp-dev \
    make \
    ninja-build \
    numactl \
    pkg-config \
    python3 \
    python3-matplotlib \
    python3-numpy \
    python3-pandas \
    python3-pip \
    tmux \
    xz-utils \
    zlib1g-dev

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

echo "Installed Docker, numactl, tmux, Python 3, matplotlib/numpy/pandas, and QEMU build dependencies."

QEMU_VERSION=6.2.0
QEMU_PREFIX=/usr/local/qemu-6.2
QEMU_BIN=/usr/local/bin/qemu-6.2-system-x86_64

if [ -x "$QEMU_BIN" ] && "$QEMU_BIN" --version | grep -q "version $QEMU_VERSION"; then
    echo "QEMU $QEMU_VERSION is already installed: $QEMU_BIN"
else
    build_dir=$(mktemp -d)
    trap 'rm -rf "$build_dir"' EXIT
    echo "Downloading and building QEMU $QEMU_VERSION from download.qemu.org."
    curl --fail --location --retry 3 --output "$build_dir/qemu-$QEMU_VERSION.tar.xz" \
        "https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"
    tar -C "$build_dir" -xf "$build_dir/qemu-$QEMU_VERSION.tar.xz"
    cd "$build_dir/qemu-$QEMU_VERSION"
    ./configure --prefix="$QEMU_PREFIX" --target-list=x86_64-softmmu --enable-kvm --disable-werror
    make -j"$(nproc)"
    $SUDO make install
    $SUDO install -m 0755 build/contrib/ivshmem-server/ivshmem-server "$QEMU_PREFIX/bin/ivshmem-server"
    $SUDO ln -sfn "$QEMU_PREFIX/bin/qemu-system-x86_64" "$QEMU_BIN"
fi

if ! "$QEMU_BIN" --version | grep -q "version $QEMU_VERSION"; then
    echo "QEMU $QEMU_VERSION installation verification failed." >&2
    exit 1
fi

echo "Installed QEMU $QEMU_VERSION as $QEMU_BIN."
