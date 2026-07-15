#!/usr/bin/env bash
# Build/configure the optional resource-util dependencies with ChCore's musl
# toolchain.  All installed files stay below the repository so chbuild's
# container sees the same absolute paths as the host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MUSL_CC="$ROOT/user/musl-1.1.24/build/bin/musl-gcc"
DEPS="$ROOT/.ae-deps"
LIBEVENT_VERSION=2.1.12
LIBEVENT_PREFIX="$DEPS/libevent-${LIBEVENT_VERSION}-musl"

if [ ! -x "$MUSL_CC" ]; then
    echo "Missing ChCore musl compiler: $MUSL_CC" >&2
    echo "Run the base x86_64 build before preparing optional workloads." >&2
    exit 1
fi
for tool in autoreconf curl make perl tar; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing host tool '$tool'; run artifact-evaluation/install-host-deps.sh" >&2
        exit 1
    }
done

mkdir -p "$DEPS"
if [ ! -f "$LIBEVENT_PREFIX/lib/libevent.a" ] || \
   [ ! -f "$LIBEVENT_PREFIX/include/event.h" ]; then
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    archive="$work/libevent.tar.gz"
    curl --fail --location --retry 3 --output "$archive" \
        "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}-stable/libevent-${LIBEVENT_VERSION}-stable.tar.gz"
    tar -C "$work" -xf "$archive"
    (
        cd "$work/libevent-${LIBEVENT_VERSION}-stable"
        CC="$MUSL_CC" ./configure \
            --build=x86_64-pc-linux-gnu \
            --host=x86_64-linux-musl \
            --prefix="$LIBEVENT_PREFIX" \
            --disable-shared --enable-static --disable-openssl
        make -j"$(nproc)"
        make install
    )
fi

configure_autotools_demo() {
    local dir="$1"
    shift
    (
        cd "$dir"
        # The bundled memcached snapshot intentionally ignores this generated
        # file, but configure.ac includes it before AC_INIT.
        if [ -f version.pl ]; then
            perl version.pl
        fi
        autoreconf -fi
        env \
            CC="$MUSL_CC" \
            AWK=awk \
            CPPFLAGS="-I$LIBEVENT_PREFIX/include" \
            LDFLAGS="-static -L$LIBEVENT_PREFIX/lib" \
            LIBS=-levent \
            ac_cv_func_clock_gettime=yes \
            ac_cv_func_malloc_0_nonnull=yes \
            ac_cv_func_realloc_0_nonnull=yes \
            pandora_cv_use_pipe=yes \
            ./configure \
                --build=x86_64-pc-linux-gnu \
                --host=x86_64-linux-musl \
                "$@"
    )
}

configure_autotools_demo "$ROOT/user/demos/memcached" \
    --with-libevent="$LIBEVENT_PREFIX" --enable-static --disable-docs
configure_autotools_demo "$ROOT/user/demos/memcachetest"

# libjpeg's checked-in generated files can otherwise try to execute a target
# probe.  An explicit host triplet makes the cross-compile contract stable.
(
    cd "$ROOT/user/demos/VeryTinyCnn/libjpeg"
    env CC="$MUSL_CC" AWK=awk \
        ./configure --build=x86_64-pc-linux-gnu --host=x86_64-linux-musl \
            --disable-shared --enable-static
)

echo "Prepared optional musl dependencies under $DEPS"
