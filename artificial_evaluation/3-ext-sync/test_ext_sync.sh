#!/bin/bash

source ../config.sh

logdir=$logbasedir/ext-sync/ext
mkdir -p $logdir

loop=(0)
intervals=(1 5 10)

for freq in ${intervals[@]}
do
    for run in ${loop[@]}
    do
        $appdir/redis.exp ckpt set 32 $freq 0 2>&1 | tee $logdir/ckpt$freq.pip32.$run.log
        sleep 10
    done
done
