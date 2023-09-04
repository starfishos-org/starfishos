#!/bin/bash

source ../config.sh
mkdir -p './result'

# incur and full checkpoint time
# data is printed and saved in "result/table3-full/incur-colum.csv"
python break_down.py $logbasedir/ckpt-breakdown/ ckpt

# restore time
# data is printed and saved in "result/table3-restore-column.csv"
python break_down_restore.py $logbasedir/restore-breakdown/ restore
