#!/bin/bash

source ../config.sh

logdir=$logbasedir/ycsb
loop=(0)
threads=(1)

mkdir -p $logdir/chcore-baseline
mkdir -p $logdir/chcore-ckpt1ms

# Test ycsb benchmark with different workload
# workload configs are given in user/demos/YCSB-C/workloads/
for workload in a b c h
do
    for thread in ${threads[@]}
    do
        for run in ${loop[@]}
        do
            # baseline
            $appdir/ycsb.exp raw $workload $thread 2>&1 | tee $logdir/chcore-baseline/$workload.chcore-raw.t$thread.$run.log
            sleep 10
            # with checkpoint
            $appdir/ycsb.exp ckpt $workload $thread 2>&1 | tee $logdir/chcore-ckpt1ms/$workload.chcore-1msckpt.t$thread.$run.log
            sleep 10
        done
    done
done
