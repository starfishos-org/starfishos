#!/bin/bash

cd tests/object
rm -rf build
mkdir build && cd build
cmake .. -G Ninja
ninja
ninja test
ret=$?
cd .. && rm -rf build
exit $ret
