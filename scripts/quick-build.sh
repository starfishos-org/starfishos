#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

# This is a script for the quick building
# ./scripts/quick-build.sh (build x86_64)
# ./scripts/quick-build.sh raspi3 (build raspi3)

if [[ $1 == *"raspi3"* ]]; then
        ./chbuild distclean
        ./chbuild defconfig raspi3 
        ./chbuild build
else
        ./chbuild distclean
        ./chbuild defconfig x86_64
        ./chbuild build
fi


