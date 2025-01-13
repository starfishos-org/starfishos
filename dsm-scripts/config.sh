sudo cpupower frequency-set -g performance > /dev/null
echo "Performance Mode Enabled"

sudo sh -c 'echo 0 > /proc/sys/kernel/numa_balancing'
echo "NUMA Balancing Disabled"

sudo modprobe msr
sudo wrmsr -a 0x1a0 0x4000850089
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
echo "Turbo boost disabled"