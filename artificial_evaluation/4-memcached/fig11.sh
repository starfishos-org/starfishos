#!/usr/bin/bash

source ../config.sh
mkdir -p ./result

# parse logs and save data in "./result/memcached-GET/SET.csv"
python read_memcached.py $logbasedir/memcached
# draw fig11 with saved data
python draw_fig11.py './result/'
echo "fig11 is saved in ./result"
