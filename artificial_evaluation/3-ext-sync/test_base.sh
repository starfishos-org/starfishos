#!/bin/bash

source ../config.sh

logdir=$logbasedir/ext-sync/base
mkdir -p $logdir

loop=(0)
intervals=(0 1 5 10)

for run in ${loop[@]}
do
    for freq in ${intervals[@]}
    do
        f=$logdir/ckpt$freq.pip32.$run.log
        if [ $freq == 0 ]; then
            $appdir/redis.exp raw set 32 2>&1 | tee $f
        else
            $appdir/redis.exp ckpt set 32 $freq 0 2>&1 | tee $f
        fi
        sleep 10
    done
done
