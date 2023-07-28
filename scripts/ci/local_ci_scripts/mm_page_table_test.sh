#!/bin/bash

cd tests/arch/aarch64
rm -rf build
mkdir build && cd build
cmake .. -G Ninja
ninja
ninja test
ret=$?
cd .. && rm -rf build
if [ ret -ne 0 ]; then
    exit $ret
fi
cd ../riscv64
rm -rf build
mkdir build && cd build
cmake .. -G Ninja
ninja
ninja test
ret=$?
cd .. && rm -rf build
if [ ret -ne 0 ]; then
    exit $ret
fi
cd ../x86_64
rm -rf build
mkdir build && cd build
cmake .. -G Ninja
ninja
ninja test
ret=$?
cd .. && rm -rf build
exit $ret
