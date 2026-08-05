#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

# This script prints a prompt and read user input.
# Should only be used in `LoadConfigAsk.cmake`.

printf '%s ' "$1"
IFS= read -r input
printf '%s\n' "$input" >&2
