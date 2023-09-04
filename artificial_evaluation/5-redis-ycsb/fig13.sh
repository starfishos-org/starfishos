#!/bin/bash

source ../config.sh
mkdir -p "./result"

# parse logs and save data in "./result/ycsb.csv"
python read_ycsb.py $logbasedir/ycsb
# draw fig13 with saved data
python draw_ycsb.py "./result/ycsb.csv" "./result/fig13.jpg"
echo "fig13 is saved in ./result"
