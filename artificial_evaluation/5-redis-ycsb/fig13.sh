#!/bin/bash

source ../config.sh

mkdir -p "./result"

python read_ycsb.py $logbasedir/ycsb
python draw_ycsb.py "./result/ycsb.csv" "./result/fig13.jpg"
