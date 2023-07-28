#!/bin/bash

source ../config.sh

mkdir -p ./result

python read_data.py $logbasedir/ext-sync/base base
python read_data.py $logbasedir/ext-sync/ext ext
python draw_fig12.py ./result/ext-sync-base.csv ./result/ext-sync-ext.csv
