#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

set -e

# return true if @v1 <= @v2
verlte() {
	[ "$1" = "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -n 1)" ]
}

verlt() {
	[ "$1" = "$2" ] && return 1 || verlte "$1" "$2"
}

vm_id=$1
qemu=$2
shift
shift
qemu_options=$@
echo "vm_id: $vm_id, qemu: $qemu, qemu_options: $qemu_options"
qemu_version_str=$($qemu --version | head -n 1)
export IFS=' '
flag="false"
qemu_version=${qemu_version_str}
for str in ${qemu_version_str}; do
	if [[ "${str}" == "version" ]]; then
		flag="true"
	elif [[ ${flag} == "true" ]]; then
		qemu_version=${str}
		break
	fi
done
unset IFS

if [[ "$qemu" == *"qemu-system-aarch64"* ]]; then
	if verlt "$qemu_version" 6.2.0; then
		# in qemu < 6.2.0, machine type = raspi3
		# in qemu >= 6.2.0, machine type = raspi3b
		qemu_options=$(echo $qemu_options | sed 's/-machine[ \t]\{1,\}raspi3b/-machine raspi3/g')
	fi
fi

# Optional host NUMA binding, enabled with CHCORE_QEMU_NUMA_BIND=1.
#
# Guest memory is already placed on purpose: dsm-scripts/config_memdev.sh binds
# each per-machine DRAM file to a CPU-bearing node and the shared CXL region to
# a CPU-less one.  The vCPU threads are not bound, so how far a guest worker
# sits from that CXL region is decided by wherever CFS happens to park its
# thread, and is redrawn on every boot.  Pinning each instance to one node makes
# the distance a property of the configuration rather than of luck.
#
# CHCORE_QEMU_NUMA_NODES optionally restricts the candidate nodes (comma or
# space separated); it defaults to every CPU-bearing node.  Machines are handed
# out over that list in order, so machine i lands on node i/ceil(machines/nodes).
#
# --preferred rather than --membind: keep allocations local while the node has
# room, but fall back to other nodes instead of inviting the OOM killer.
numa_prefix=()
if [ "${CHCORE_QEMU_NUMA_BIND:-0}" = "1" ] && command -v numactl >/dev/null 2>&1 &&
	[ "$vm_id" -ge 0 ] 2>/dev/null; then
	if [ -n "${CHCORE_QEMU_NUMA_NODES:-}" ]; then
		numa_nodes=(${CHCORE_QEMU_NUMA_NODES//,/ })
	else
		numa_nodes=($(numactl --hardware |
			awk '/^node [0-9]+ cpus:/ && NF > 3 { print $2 }'))
	fi
	machine_num=$(printf '%s' "$qemu_options" |
		grep -oE 'machine_num=[0-9]+' | head -n 1 | cut -d= -f2)
	: "${machine_num:=1}"
	nnodes=${#numa_nodes[@]}
	if [ "$nnodes" -gt 0 ]; then
		per_node=$(((machine_num + nnodes - 1) / nnodes))
		[ "$per_node" -lt 1 ] && per_node=1
		idx=$((vm_id / per_node))
		[ "$idx" -ge "$nnodes" ] && idx=$((nnodes - 1))
		node=${numa_nodes[$idx]}
		numa_prefix=(numactl --cpunodebind="$node" --preferred="$node")
		echo "[NUMA] vm_id=$vm_id -> host node $node" \
			"(machines=$machine_num, nodes=${numa_nodes[*]})"
	fi
fi

set +e
"${numa_prefix[@]}" $qemu $qemu_options
qemu_rc=$?
set -e
echo "[QEMU-EXIT] vm_id=$vm_id rc=$qemu_rc"
exit "$qemu_rc"
