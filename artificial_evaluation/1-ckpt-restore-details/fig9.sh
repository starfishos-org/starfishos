#!/usr/bin/bash

source ../config.sh

python draw_fig.py $logbasedir/ckpt-breakdown/ a
python draw_fig.py $logbasedir/ckpt-breakdown/ b
echo "fig9a and fig9b is saved in ./result"
