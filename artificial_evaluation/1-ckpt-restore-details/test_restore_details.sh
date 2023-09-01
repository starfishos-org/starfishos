#!/bin/bash

source ../config.sh

logdir=$logbasedir/restore-breakdown
loop=(0)
mkdir -p $logdir

for workload in default word_count sqlite leveldb kmeans redis memcached
# run a subset of workload
# for workload in leveldb kmeans redis memcached
do
    for run in ${loop[@]}
    do
        # $appdir/$workload.exp "restore-log" 500 4 2>&1 | tee $logdir/$workload.restore.$run.log
        if [ $workload == "redis" ]; then
        $appdir/$workload.exp restore-test set nopipe 1000 0 2>&1 | tee $logdir/$workload.restore.$run.log
        elif [ $workload == "leveldb" ]; then
        $appdir/$workload.exp "restore-test" 100 0 2>&1 | tee $logdir/$workload.restore.$run.log
        else
        $appdir/$workload.exp "restore-test" 1000 0 2>&1 | tee $logdir/$workload.restore.$run.log
        fi
        sleep 5
    done
done