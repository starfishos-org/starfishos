#!/bin/bash

source ../config.sh

mkdir -p ./result

# parse logs and save data in "./result/hybrid-mem.csv"
python read_data.py $logbasedir/hybrid-mem
# draw fig10 with saved data
python draw_fig10.py ./result/hybrid-mem.csv
echo "fig10 is saved in ./result"
