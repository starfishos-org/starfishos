#!/bin/bash

source ../config.sh
mkdir -p './result'

# data is printed and saved in "result/table4.csv"
# data will have five rows:
# 1. # of runtime page faults
# 2. # of dirty cached pages
# 3. # of cached pages
# 4. Ratio of page faults eliminated
# 5. Dirty rate in cached pages
python break_down.py $logbasedir/ckpt-breakdown/ extra
