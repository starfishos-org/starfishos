#!/bin/sh
{
    flock -n 9
    if [ $? -ne 0 ]; then
        echo "board is in use, exit!"
        exit 1
    fi

    scp kernel/arch/aarch64/boot/raspi3/firmware/* pi@chos-pi-ci:/home/pi/tftpboot
    scp build/kernel8.img pi@chos-pi-ci:/home/pi/tftpboot
    ssh pi@chos-pi-ci "./reset_ch1.py"
    ./scripts/ci/expect_wrapper.sh $1
    res=$?
    flock -u 9
    if [ ${res} -ne 0 ]; then
        exit ${res}
    fi
} 9<>./scripts/ci/raspi.lock
