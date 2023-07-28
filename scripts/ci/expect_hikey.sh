#!/bin/sh

num=0
ch=0
device0=71B55F2D02890537
device1=E166D59028B66FD

sudo ./build/build_rootfs_img.sh

while :
do
    echo try lock /tmp/chos$num
    exec 9>/tmp/chos$num
    flock -n 9
    if [ $? -eq 0 ]; then
        ch=$((2*num+1))
        ssh chcore@chcore-pi "sudo ./reset_ch$ch.py"
        sleep 3
        device=`eval echo '$'device$num`
        sudo fastboot -s ${device} flash system ./build/rootfs.img
        ./scripts/ci/expect_wrapper.sh $1 $num
        ret=$?
        flock -u 9
        exec 9<&-
        exit $ret
    else
        exec 9<&-
        sleep 10
        num=`expr 1 - $num`
    fi
done
