#!/bin/bash
pmem_dir=/mnt/treesls
pmem_dev=/dev/pmem0
sudo mkdir -p $pmem_dir
sudo rm -rf $pmem_dir/*
sudo umount $pmem_dir
sudo mkfs.ext4 -F $pmem_dev
#sudo mkfs.xfs -f -m reflink=0 $pmem_dev
sudo mount -o dax $pmem_dev $pmem_dir
