sudo cpupower frequency-set -g performance > /dev/null
echo "Performance Mode Enabled"

echo 0 >/proc/sys/kernel/numa_balancing
echo "NUMA Balancing Disabled"