#!/bin/bash

source ../config.sh

mkdir -p ./result

python read_data.py $logbasedir/hybrid-mem/
python draw_fig10.py ./result/hybrid-mem.csv
