#!/bin/bash

source ../config.sh

logdir=$logbasedir/ext-sync/base
mkdir -p $logdir

loop=(0)
intervals=(0 1 5 10)

# Test redis set benchmark with different checkpoint interval
for freq in ${intervals[@]}
do
    mkdir -p $logdir/$freq
    for run in ${loop[@]}
    do
        # checkpoint interval == 0 means raw mode without checkpointing 
        if [ $freq == 0 ]; then
            $appdir/redis.exp raw set 32 2>&1 | tee $logdir/ckpt0.pip32.$run.log
        else
            $appdir/redis.exp ckpt set 32 $freq 0 2>&1 | tee $logdir/ckpt$freq.pip32.$run.log
        fi
    done
done
