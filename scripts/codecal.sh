#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

git diff master --ignore-space-change --ignore-all-space --ignore-blank-lines --stat -- 'kernel' ':!kernel/tests' ':!kernel/drivers' ':!kernel/include/drivers' ':!kernel/arch/x86_64/drivers/' ':!kernel/ckpt-ssi' ':!kernel/ckpt/' ':!kernel/dsm/' | cat

git diff master --ignore-space-change --ignore-all-space --ignore-blank-lines --stat -- 'kernel' ':!kernel/tests' ':!kernel/drivers' ':!kernel/include/drivers' ':!kernel/arch/x86_64/drivers/' | cat

git diff master --ignore-space-change --ignore-all-space --ignore-blank-lines --stat -- 'user' ':!user/demos' ':!user/script' ':!user/sample-apps/' | cat
