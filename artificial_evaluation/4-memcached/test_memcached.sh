#!/bin/bash

source ../config.sh

logdir=$logbasedir/memcached
mkdir -p $logdir

loop=(0)
intervals=(0 1 5 10 50)

# Test memcached benchmark with different checkpoint interval
for freq in ${intervals[@]}
do
    for run in ${loop[@]}
    do
        f=t8.$run.log
        # checkpoint interval == 0 means raw mode without checkpointing 
        if [ $freq == 0 ]; then
            $appdir/memcached.exp raw 2>&1 | tee $logdir/ckpt0.$f
        else
            $appdir/memcached.exp ckpt $freq 0 2>&1 | tee $logdir/ckpt$freq.$f
        fi
    done
done
