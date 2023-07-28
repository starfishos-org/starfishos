#!/bin/bash

source ../config.sh
mkdir -p './result'

python break_down.py $logbasedir/ckpt-breakdown/ extra
