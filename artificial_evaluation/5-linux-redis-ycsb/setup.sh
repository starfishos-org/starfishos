#!/bin/bash

curdir=$(pwd)

# install musl-libc-1.1.24
musldir="$curdir/musl-1.1.24"
if [ ! -d $musldir ]; then
    # dowload
    wget https://musl.libc.org/releases/musl-1.1.24.tar.gz
    tar xvf $curdir/musl-1.1.24.tar.gz
    # install
    cd $musldir
    ./configure
    make
    sudo make install
    cd $curdir
fi

# make redis
cd $curdir/redis
git checkout 6.0.8
git apply $curdir/redis-musl.patch
make MALLOC=libc CC=musl-gcc -j12
cd $curdir

# make YCSB-C
cd $curdir/YCSB-C
# remove tbb and enable unixsocket
git apply $curdir/ycsb.patch
make
cp $curdir/workloadg.spec $curdir/YCSB-C/workloads/
