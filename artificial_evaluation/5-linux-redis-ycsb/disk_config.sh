#!/bin/bash
disk_dir=/mnt/treesls
disk_dev=/dev/nvme0n1p1
sudo mkdir -p $disk_dir
sudo umount $disk_dir
sudo mount $disk_dev $disk_dir
