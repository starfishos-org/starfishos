#!/bin/bash

# Unset configuration
if [ "$1" == "unset" ]; then
    sudo cpupower frequency-set -g performance
    echo "Performance Mode"

    sudo sh -c 'echo 1 > /proc/sys/kernel/numa_balancing'
    echo "NUMA Balancing Enabled"

    # Enable Turbo boost
    if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
        echo 1 | sudo tee /sys/devices/system/cpu/cpufreq/boost > /dev/null
        echo "Turbo boost enabled"
    elif [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
        echo 0 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null 
        echo "Turbo boost enabled"
    else
        echo "Turbo boost not supported"
    fi
    exit 1
fi

# Set configuration
sudo cpupower frequency-set -f 2.10GHz > /dev/null
echo "Performance Fixed to 2.10GHz"

sudo sh -c 'echo 0 > /proc/sys/kernel/numa_balancing'
echo "NUMA Balancing Disabled"

# Disable Turbo boost
if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost > /dev/null
    echo "Turbo boost disabled"
elif [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    # sudo modprobe msr
    # sudo wrmsr -a 0x1a0 0x4000850089
    echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
    echo "Turbo boost disabled"
else
    echo "Turbo boost not supported"
fi
exit 0