#!/bin/bash

source ../config.sh

logdir=$logbasedir/restore-breakdown
loop=(0)
mkdir -p $logdir

# Test each workloads with restore-test mode
# restore-test mode will first take a checkpoint and then shutdown to restore
for workload in default word_count sqlite leveldb kmeans redis memcached
# run a subset of workload
# for workload in default
do
    for run in ${loop[@]}
    do
        echo "Test restore details with workload: $workload"
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