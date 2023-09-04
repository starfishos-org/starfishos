#!/bin/bash

source ../config.sh
mkdir -p ./result

# parse logs in $logbasedir/ext-sync/base and ext
# save data in ./result/ext-sync-base.csv and ./result/ext-sync-ext.csv
python read_data.py $logbasedir/ext-sync/base base
python read_data.py $logbasedir/ext-sync/ext ext
# draw fig12 with saved data
python draw_fig12.py ./result/ext-sync-base.csv ./result/ext-sync-ext.csv
echo "fig12 is saved in ./result"
