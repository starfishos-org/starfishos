#!/bin/bash

source ../config.sh
mkdir -p ./result

python read_rocksdb.py $logbasedir/rocksdb
python draw_rocksdb.py "./result/rocksdb.csv"
echo "fig14(a,b,c,d) are saved in ./result"
