#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
cpu_num=$1
# phoenix_cpu_num=$2

sed -i "s/^#define PLAT_CPU_NUM.*/#define PLAT_CPU_NUM ($cpu_num)/g" kernel/include/arch/x86_64/plat/intel/machine.h
sed -i "s/plat_cpu_name=.*/plat_cpu_name=$cpu_num/g" scripts/qemu/emulate.tpl.sh
# sed -i "s/^#define PHOENIX_CPU_NUM.*/#define PHOENIX_CPU_NUM ($phoenix_cpu_num)/g" user/demos/phoenix-2.0/src/processor.c
