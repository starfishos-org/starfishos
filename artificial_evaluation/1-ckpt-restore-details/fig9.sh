#!/bin/bash

source ../config.sh

python draw_fig.py $logbasedir/ckpt-breakdown/ a
python draw_fig.py $logbasedir/ckpt-breakdown/ b
