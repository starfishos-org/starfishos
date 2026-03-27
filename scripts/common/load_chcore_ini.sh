#!/bin/bash

# Load global config from project-root chcore.ini.
# Exported variables:
#   CHCORE_INI_CPU_NUM
#   CHCORE_INI_MACHINE_NUM
#   CHCORE_INI_DRAM_SIZE
#   CHCORE_INI_CXL_SIZE

_chcore_ini_trim() {
	local s="$1"
	s="${s#"${s%%[![:space:]]*}"}"
	s="${s%"${s##*[![:space:]]}"}"
	printf '%s' "$s"
}

_chcore_ini_get() {
	local file="$1"
	local key="$2"
	awk -F '=' -v k="$key" '
		/^[[:space:]]*#/ { next }
		/^[[:space:]]*;/ { next }
		/^[[:space:]]*$/ { next }
		/^[[:space:]]*\[/ { next }
		{
			key=$1
			val=substr($0, index($0, "=") + 1)
			gsub(/^[ \t]+|[ \t]+$/, "", key)
			gsub(/^[ \t]+|[ \t]+$/, "", val)
			if (key == k) {
				print val
				exit
			}
		}
	' "$file"
}

load_chcore_ini() {
	local script_dir project_root ini_file
	script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	project_root="$(cd "$script_dir/../.." && pwd)"
	ini_file="$project_root/chcore.ini"

	if [ ! -f "$ini_file" ]; then
		return 0
	fi

	CHCORE_INI_CPU_NUM="$(_chcore_ini_trim "$(_chcore_ini_get "$ini_file" "cpu_num")")"
	CHCORE_INI_MACHINE_NUM="$(_chcore_ini_trim "$(_chcore_ini_get "$ini_file" "machine_num")")"
	CHCORE_INI_DRAM_SIZE="$(_chcore_ini_trim "$(_chcore_ini_get "$ini_file" "dram_size")")"
	CHCORE_INI_CXL_SIZE="$(_chcore_ini_trim "$(_chcore_ini_get "$ini_file" "cxl_size")")"

	export CHCORE_INI_CPU_NUM
	export CHCORE_INI_MACHINE_NUM
	export CHCORE_INI_DRAM_SIZE
	export CHCORE_INI_CXL_SIZE
}
