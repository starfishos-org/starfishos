#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

source ./test-scripts/config.sh

./dsm-scripts/change_cpu_num.sh 64
cd $basedir
./scripts/quick-build.sh
