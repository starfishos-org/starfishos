#!/bin/bash

make start-ivshmem-server
make clean-dsm
./build/simulate.sh 0 | tee exec_log.log
make kill-ivshmem-server