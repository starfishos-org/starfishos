#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

rm -rf build && mkdir build

cd build
cmake .. -G Ninja
ninja && ctest && ninja lcov && echo -e "\n\nPlease open ./build/report/index.html for the coverage report"
