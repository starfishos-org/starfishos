#!/bin/bash

set -e

# basedir will be the root-level build dir
basedir=$(dirname "$0")

dd if=/dev/zero of=$basedir/rootfs.img bs=1M count=64 conv=sparse
mkfs.ext4 -L rootfs $basedir/rootfs.img
rm -rf $basedir/mnt
mkdir $basedir/mnt
sudo mount $basedir/rootfs.img $basedir/mnt
sudo cp -r $basedir/rootfs/* $basedir/mnt
sudo umount $basedir/mnt
rm -r $basedir/mnt
