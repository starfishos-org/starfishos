#!/bin/bash

source ../config.sh
logdir=$logbasedir/rocksdb/
loop=(0)

mkdir -p $logdir

for run in ${loop[@]}
do
    # baseline 
    mkdir -p $logdir/chcore-base
    $appdir/rocksdb.exp raw 2>&1 | tee $logdir/chcore-base/$run.out
    sleep 10

    # baseline with WAL
    # mkdir -p $logdir/chcore-base-wal
    # $appdir/rocksdb.exp wal 2>&1 | tee $logdir/chcore-base-wal/$run.out
    # sleep 10

    # with ckpt
    mkdir -p $logdir/chcore-ckpt
    $appdir/rocksdb.exp ckpt 1 0 2>&1 | tee $logdir/chcore-ckpt/$run.out
    sleep 10
done