#!/bin/bash
disk_dir=/mnt/treesls
disk_dev=/dev/nvme0n1p1
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
sudo umount $disk_dir
sudo mount $disk_dev $disk_dir
