#!/bin/bash

source ../config.sh

logdir=$logbasedir/memcached
mkdir $logdir

loop=(20 21 22)
intervals=(0 1 5 10 50)

for freq in ${intervals[@]}
do
    for run in ${loop[@]}
    do
        f=$logdir/ckpt$freq.t8.$run.log
        if [ $freq == 0 ]; then
            $appdir/memcached.exp raw 2>&1 | tee $f
        else
            $appdir/memcached.exp ckpt $freq 0 2>&1 | tee $f
        fi
        sleep 10
    done
done
