#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi

bashdir=/home/wfn/chcore-cxl/
file_path=/disk/wfn/models/Meta-Llama-3-8B-Instruct.Q5_K_M.gguf

cd $bashdir
# Create a 40GB empty file
# dd if=/dev/zero of=disk.img bs=1G count=40
# Put the file into the image (mount and copy)
sudo mkfs.ext4 disk.img
sudo mount -o loop disk.img /mnt
sudo cp $file_path /mnt/
sudo umount /mnt
