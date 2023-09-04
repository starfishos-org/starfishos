#!/bin/bash

source ../config.sh
mkdir -p ./result

# parse logs and save data in "./result/rocksdb.csv"
python read_rocksdb.py $logbasedir/rocksdb/1/
# draw fig14 with saved data
python draw_rocksdb.py "./result/rocksdb.csv"
echo "fig14(a,b,c,d) are saved in ./result"
