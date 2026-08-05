#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

make start-ivshmem-server
make clean-dsm
sleep 3
./build/simulate.sh 0 | tee exec_log.log
make kill-ivshmem-server