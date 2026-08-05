#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

source ./test-scripts/config.sh

sed -i \
    -e 's/DSM_HEAP_MODE = CXL/DSM_HEAP_MODE = DRAM/' \
    $basedir/user/musl-1.1.24/Makefile

echo "config to shared_data_cxl_only completed"
