#!/bin/bash

source ../config.sh

logdir=$logbasedir/ext-sync/base
mkdir -p $logdir

loop=(0)
intervals=(1 5 10)

$appdir/redis.exp raw set 32 2>&1 | tee $logdir/ckpt0.pip32.$run.log

for freq in ${intervals[@]}
do
    mkdir -p $logdir/$freq
    for run in ${loop[@]}
    do
        $appdir/redis.exp ckpt set 32 $freq 0 2>&1 | tee $logdir/ckpt$freq.pip32.$run.log
    done
done
