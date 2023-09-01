#!/bin/bash

source ../config.sh

logdir=$logbasedir/ycsb
loop=(0)
threads=(1)

mkdir -p $logdir
mkdir -p $logdir/chcore-baseline
mkdir -p $logdir/chcore-ckpt1ms

for workload in a b c g
do
    for thread in ${threads[@]}
    do
        for run in ${loop[@]}
        do
            $appdir/ycsb.exp raw $workload $thread 2>&1 | tee $logdir/chcore-baseline/$workload.chcore-raw.t$thread.$run.log
            sleep 10

            $appdir/ycsb.exp ckpt $workload $thread 2>&1 | tee $logdir/chcore-ckpt1ms/$workload.chcore-1msckpt.t$thread.$run.log
            sleep 10
        done
    done
done
