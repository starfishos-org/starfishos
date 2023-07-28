#!/bin/bash

source ../config.sh

logdir=$logbasedir/hybrid-mem
loop=(0)
mode=$1

mkdir -p $logdir

for workload in memcached
do
    mkdir -p $logdir/$workload
    for run in ${loop[@]}
    do
        f=$logdir/$workload/$mode.$run.log
        if [ $workload == "redis" ]; then
            $appdir/$workload.exp ckpt set nopipe 1 0 2>&1 | tee $f
        else 
            $appdir/$workload.exp ckpt 1 0 2>&1 | tee $f
        fi
    done
done