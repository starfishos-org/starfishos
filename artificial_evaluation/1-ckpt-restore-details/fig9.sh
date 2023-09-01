#!/bin/bash

source ../config.sh
mkdir -p './result'

python draw_fig.py $logbasedir/ckpt-breakdown/ a
python draw_fig.py $logbasedir/ckpt-breakdown/ b
echo "fig9a and fig9b is saved in ./result"