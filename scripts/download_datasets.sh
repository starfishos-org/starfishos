#!/bin/bash
# Download large benchmark datasets (phoenix data files, GeminiGraph twitter-2010
# graph) from the public HF dataset mirror into <repo-root>/datasets/.
#
# These files are gitignored and referenced by symlinks from
# test-on-linux/phoenix/data/... and user/demos/phoenix-2.0/data/..., and by
# dsm-scripts/prepare_hostfs.py (twitter-2010.bin). Run standalone or via
# `make prepare`.
#
# Env vars:
#   SKIP_GRAPH_DATASET=1   skip twitter-2010.bin (11GB, only needed for GeminiGraph)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATASETS_DIR="$REPO_ROOT/datasets"
HF_BASE="https://huggingface.co/datasets/ShadowNearby/starfishos-datasets/resolve/main"

mkdir -p "$DATASETS_DIR"

# name:expected_size_bytes
files="
key_file_500MB.txt:542454118
key_file_100MB.txt:108490822
key_file_50MB.txt:57557323
word_100MB.txt:105311535
word_50MB.txt:52472758
matrix_file_A.txt:100000000
matrix_file_B.txt:100000000
"

if [ "${SKIP_GRAPH_DATASET:-0}" != "1" ]; then
    files="$files
twitter-2010.bin:11746921456"
fi

for entry in $files; do
    name="${entry%%:*}"
    expected_size="${entry##*:}"
    dest="$DATASETS_DIR/$name"

    if [ -f "$dest" ] && [ "$(stat -c%s "$dest" 2>/dev/null || stat -f%z "$dest")" = "$expected_size" ]; then
        echo "skip (already present): $name"
        continue
    fi

    echo "downloading: $name ($expected_size bytes)"
    tmp="$dest.part"
    curl -fL --retry 3 -C - -o "$tmp" "$HF_BASE/$name"

    actual_size="$(stat -c%s "$tmp" 2>/dev/null || stat -f%z "$tmp")"
    if [ "$actual_size" != "$expected_size" ]; then
        echo "error: $name downloaded size $actual_size != expected $expected_size" >&2
        exit 1
    fi
    mv "$tmp" "$dest"
done

echo "All datasets present in $DATASETS_DIR"
