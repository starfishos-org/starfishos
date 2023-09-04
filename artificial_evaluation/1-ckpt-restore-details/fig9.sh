#!/bin/bash

source ../config.sh
mkdir -p './result'

# draw fig9a and fig9b with logs in $logbasedir/ckpt-breakdown/ 
python draw_fig.py $logbasedir/ckpt-breakdown/ a
python draw_fig.py $logbasedir/ckpt-breakdown/ b

echo "fig9a and fig9b is saved in ./result"
