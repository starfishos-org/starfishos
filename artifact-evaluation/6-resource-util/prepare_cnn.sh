#!/usr/bin/env bash
# Materialize the deterministic TinyCNN inputs omitted by the historical
# submodule.  Zero-filled AlexNet weights preserve the exact layer shapes and
# compute/memory path needed by this performance experiment; accuracy is not a
# measured quantity in the paper's resource-utilization figure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CNN="$ROOT/user/demos/VeryTinyCnn"

if [ ! -f "$CNN/CMakeLists.txt" ] || [ ! -f "$CNN/libjpeg/Makefile" ]; then
    echo "TinyCNN submodule is missing. Run:" >&2
    echo "  git submodule update --init --recursive user/demos/VeryTinyCnn" >&2
    exit 1
fi

mkdir -p "$CNN/data/pca" "$CNN/image"
truncate -s 228015360 "$CNN/data/alexnet.dat"
truncate -s 81920 "$CNN/data/pca/nn-004.dat"
cp "$CNN/libjpeg/testimgp.jpg" "$CNN/image/ae-input.jpg"

: > "$CNN/data/filelists.txt"
for i in $(seq 1 8); do
    printf 'image/ae-input.jpg\t0\n' >> "$CNN/data/filelists.txt"
done
cut -f 1 "$CNN/data/filelists.txt" > "$CNN/data/raw_filelists.txt"

echo "Prepared deterministic TinyCNN model + 8-image batch under $CNN/data"
