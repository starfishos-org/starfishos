#!/bin/bash

source ../config.sh
mkdir -p './result'

# object composition (count)
# data is printed and saved in "result/table2-counts.csv"
python break_down.py $logbasedir/ckpt-breakdown/ count

# size (MB)
# data is printed and saved in "result/table2-memsize.csv"
python parse_mem_size.py $logbasedir/ckpt-size/ './result/table2-mem-size.csv'
