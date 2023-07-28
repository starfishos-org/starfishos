#!/bin/bash

source ../config.sh
mkdir -p './result'

python break_down.py $logbasedir/ckpt-breakdown/ ckpt
python break_down_restore.py $logbasedir/restore-breakdown/ restore
