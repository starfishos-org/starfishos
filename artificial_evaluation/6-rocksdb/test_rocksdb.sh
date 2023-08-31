#!/bin/bash

source ../config.sh
logdir=$logbasedir/rocksdb/
loop=(0)

mkdir -p $logdir

for run in ${loop[@]}
do
    for run in ${loop[@]}
    do
        baseline
        mkdir -p $logdir/$thread/chcore-base
        $appdir/rocksdb.exp raw $thread 2>&1 | tee $logdir/$thread/chcore-base/$run.out
        sleep 10
    done
    for run in ${loop[@]}
    do
        # with ckpt
        mkdir -p $logdir/chcore-ckpt
        $appdir/rocksdb.exp ckpt $thread 1 0 2>&1 | tee $logdir/$thread/chcore-ckpt/$run.out
        sleep 10
    done
done
