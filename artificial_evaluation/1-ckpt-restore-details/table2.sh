#!/bin/bash

source ../config.sh

mkdir -p './result'

# Object Composition (Count)
python break_down.py $logbasedir/ckpt-breakdown/ count

# Size (MB)
python parse_mem_size.py $logbasedir/ckpt-size/ './result/mem_size.csv'
