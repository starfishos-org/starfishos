#!/bin/bash
pmem_dir=/mnt/treesls
pmem_dev=/dev/pmem0
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
sudo rm -rf $pmem_dir/*
sudo umount $pmem_dir
sudo mkfs.ext4 -F -b 4096 $pmem_dev
sudo mount -o dax $pmme_dev $pmem_dir
