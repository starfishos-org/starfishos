#!/bin/bash

SCRIPT_DIR=$(dirname "$0")
SOURCE_DIR="$SCRIPT_DIR/../.."
HEADER_DIR="${SOURCE_DIR}/kernel/include/arch/aarch64/arch/virt"

if test -f "${HEADER_DIR}/asm-offsets.h"; then
	rm ${HEADER_DIR}/asm-offsets.h
fi

echo "#pragma once\n" >> ${HEADER_DIR}/asm-offsets.h
echo "/*This file is generated. Please do not modify it!*/\n" >> ${HEADER_DIR}/asm-offsets.h
sed -f ${SOURCE_DIR}/scripts/virt/asm-offsets.sed < ${HEADER_DIR}/asm-offsets.s >> ${HEADER_DIR}/asm-offsets.h
rm ${HEADER_DIR}/asm-offsets.s
