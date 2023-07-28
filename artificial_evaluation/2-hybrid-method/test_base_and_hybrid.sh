#!/bin/bash

source ../config.sh

logdir=$logbasedir/hybrid-mem
loop=(0)

mkdir -p $logdir

for workload in memcached redis kmeans pca
do
    mkdir -p $logdir/$workload
    for run in ${loop[@]}
    do
        # test base (no checkpoint)
        f1=$logdir/$workload/raw.$run.log
        if [ $workload == "redis" ]; then
            $appdir/$workload.exp raw set nopipe 2>&1 | tee $f1
        else 
            $appdir/$workload.exp raw 2>&1 | tee $f1
        fi
        sleep 5
        
        # test hybrid memory checkpoint
        f2=$logdir/$workload/ckpt1ms.with-migration.$run.log
        if [ $workload == "redis" ]; then
            $appdir/$workload.exp ckpt set nopipe 1 0 2>&1 | tee $f2
        else 
            $appdir/$workload.exp ckpt 1 0 2>&1 | tee $f2
        fi
        sleep 5
    done
done