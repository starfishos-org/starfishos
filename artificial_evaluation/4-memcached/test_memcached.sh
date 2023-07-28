#!/bin/bash

source ../config.sh

logdir=$logbasedir/memcached
mkdir $logdir

loop=(0)
intervals=(1 5 10 50)

for freq in ${intervals[@]}
do
    for run in ${loop[@]}
    do
        f=t8.$run.log
        $appdir/memcached.exp raw 2>&1 | tee $logdir/ckpt0.$f
        sleep 10

        $appdir/memcached.exp ckpt $freq 0 2>&1 | tee $logdir/ckpt$freq.$f
        sleep 10
    done
done
