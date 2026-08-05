#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

./dsm-scripts/config.sh
./dsm-scripts/config_memdev.sh cxl
numactl --cpunodebind=0 ./build/simulate.sh

matrix_multiply.bin 2000 2000 -t=1
matrix_multiply.bin 100 100 -t=1
matrix_multiply_cxl.bin 2000 2000 -t=1
matrix_multiply_cxl.bin 100 100 -t=1
