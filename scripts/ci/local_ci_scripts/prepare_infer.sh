#!/bin/bash

CHCORE_PROJECT_DIR=$(pwd)

rm -f infer_kernel_report.txt

rm -rf kernel/build
cmake -S kernel -B kernel/build \
        -DCMAKE_MODULE_PATH=$CHCORE_PROJECT_DIR/scripts/build/cmake/Modules \
        -DCMAKE_TOOLCHAIN_FILE=$CHCORE_PROJECT_DIR/scripts/build/cmake/Toolchains/kernel_fbinfer.cmake \
        -DCHCORE_PROJECT_DIR=$CHCORE_PROJECT_DIR \
        -DCHCORE_USER_INSTALL_DIR= \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=1
