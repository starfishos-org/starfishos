#!/usr/bin/env bash
# Materialize the deterministic TinyCNN inputs omitted by the historical
# submodule.  Zero-filled AlexNet weights preserve the exact layer shapes and
# compute/memory path needed by this performance experiment; accuracy is not a
# measured quantity in the paper's resource-utilization figure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CNN="$ROOT/user/demos/VeryTinyCnn"

if [ ! -f "$CNN/CMakeLists.txt" ] || [ ! -x "$CNN/libjpeg/configure" ]; then
    echo "TinyCNN submodule is missing. Run:" >&2
    echo "  git submodule update --init --recursive user/demos/VeryTinyCnn" >&2
    exit 1
fi

# VeryTinyCnn intentionally ignores this historical header.  Fetch the exact
# version used by the original artifact instead of relying on an untracked
# file left in a developer checkout.
if [ ! -s "$CNN/include/CImg.h" ]; then
    tmp_archive="$(mktemp)"
    tmp_header="$(mktemp "$CNN/include/CImg.h.XXXXXX")"
    trap 'rm -f "$tmp_archive" "$tmp_header"' EXIT
    curl --fail --location --retry 3 \
        --output "$tmp_archive" \
        https://github.com/GreycLab/CImg/archive/refs/tags/v1.6.5.tar.gz
    tar -xOf "$tmp_archive" CImg-1.6.5/CImg.h > "$tmp_header"
    if [ ! -s "$tmp_header" ]; then
        echo "Downloaded CImg archive did not contain CImg.h" >&2
        exit 1
    fi
    mv "$tmp_header" "$CNN/include/CImg.h"
fi

mkdir -p "$CNN/data/pca" "$CNN/image"
truncate -s 228015360 "$CNN/data/alexnet.dat"
truncate -s 81920 "$CNN/data/pca/nn-004.dat"
cp "$CNN/libjpeg/testimgp.jpg" "$CNN/image/ae-input.jpg"

: > "$CNN/data/filelists.txt"
for i in $(seq 1 8); do
    printf 'image/ae-input.jpg\t0\n' >> "$CNN/data/filelists.txt"
done

echo "Prepared deterministic TinyCNN model + 8-image batch under $CNN/data"
