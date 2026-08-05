#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

mode="default"
mode=$1

disable_prefetchers() {
	echo "Disabling hardware prefetchers..."
	for cpu in $(ls /dev/cpu/); do
		current_value=$(sudo rdmsr -p $cpu 0x1A4)
		new_value=$((current_value | 0xF))
		sudo wrmsr -p $cpu 0x1A4 $new_value
	done
	echo "Hardware prefetchers disabled."
}

restore_prefetchers() {
	echo "Restoring hardware prefetchers..."
	for cpu in $(ls /dev/cpu/); do
		current_value=$(sudo rdmsr -p $cpu 0x1A4)
		new_value=$((current_value & ~0xF))
		sudo wrmsr -p $cpu 0x1A4 $new_value
	done
	echo "Hardware prefetchers restored."
}

if [ "$mode" == "disable" ]; then
	disable_prefetchers
	exit 0
fi
numactl --cpunodebind=0 --membind=0 ./mem_perf_checker

if [ "$mode" == "enable" ]; then
	restore_prefetchers
	exit 0
fi
