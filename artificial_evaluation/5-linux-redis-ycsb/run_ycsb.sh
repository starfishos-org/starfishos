#!/bin/bash
set -x
ycsb_dir=./YCSB-C
workload=$1
thread=$2
run=$3
log_dir=./logs/$4
mkdir -p $log_dir
sudo taskset -c 2,4,6,8,10,12,14,16,18,20 $ycsb_dir/ycsbc -db redis -threads $thread -P $ycsb_dir/workloads/workload$workload.spec -host /tmp/redis.sock -port 0 -slaves 0 2>&1 | tee $log_dir/$workload.linux.t$thread.$run.log
