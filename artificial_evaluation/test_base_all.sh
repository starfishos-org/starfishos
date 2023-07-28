#!/usr/bin/bash

curdir=$(pwd)

cd $curdir/2-hybrid-method
./test_base_and_hybrid.sh

cd $curdir/3-ext-sync
./test_base.sh

cd $curdir/4-memcached
./test_memcached.sh

cd $curdir/5-ycsb
./test_ycsb.sh

cd $curdir/6-rocksdb
./test_rocksdb.sh
