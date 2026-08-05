#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

source ./test-scripts/config.sh

sed -i \
    -e 's/set(DSM_MALLOC_MODE "CXL")/set(DSM_MALLOC_MODE "MIXED")/' \
    -e 's/set(DSM_PGTABLE_MODE "CXL")/set(DSM_PGTABLE_MODE "DRAM")/' \
    $basedir/kernel/dsm_config.cmake

echo "config to pgtable_dram_only completed"
