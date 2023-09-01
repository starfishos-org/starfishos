#!/usr/bin/bash

source ../config.sh
mkdir -p ./result

python read_memcached.py $logbasedir/memcached
python draw_fig11.py './result/'
echo "fig11 is saved in ./result"
