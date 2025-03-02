#!/bin/bash

bashdir=/home/wfn/chcore-cxl/
file_path=/disk/wfn/models/Meta-Llama-3-8B-Instruct.Q5_K_M.gguf

cd $bashdir
# 创建一个40GB的空文件
# dd if=/dev/zero of=disk.img bs=1G count=40
# 将文件放入镜像（需挂载并复制）
sudo mkfs.ext4 disk.img
sudo mount -o loop disk.img /mnt
sudo cp $file_path /mnt/
sudo umount /mnt