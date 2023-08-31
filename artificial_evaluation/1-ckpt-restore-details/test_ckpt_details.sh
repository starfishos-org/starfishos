#!/bin/bash

source ../config.sh

logdir=$logbasedir/ckpt-breakdown
loop=(0)
mkdir -p $logdir

for workload in default memcached redis sqlite leveldb kmeans word_count pca
# run a subset of workload
# for workload in kmeans
do
    for run in ${loop[@]}
    do
        if [ $workload == "redis" ]; then
        $appdir/$workload.exp ckpt-log set nopipe 1 3 2>&1 | tee $logdir/$workload.ckpt1ms.log3.$run.log
        else
        $appdir/$workload.exp ckpt-log 1 3 2>&1 | tee $logdir/$workload.ckpt1ms.log3.$run.log
        fi
        sleep 5
    done
done