#!/bin/bash
loop=(0)
threads=(1)

restart()
{
	mode=$1
	kill -9 $(pidof redis-server)
	sleep 5
	if [ $mode = "nvm-log" ]; then
		./dax_config.sh
	elif [ $mode = "disk-log" ]; then
		./disk_config.sh
	else
		./config.sh
	fi
	sleep 5
}

for mode in "baseline" "nvm-log" "disk-log"
#for mode in "nvm-log" "disk-log"
do
for workload in a b c g
do
	for thread in ${threads[@]}
	do
		for run in ${loop[@]}	
		do
		restart $mode
		./run_redis_server.sh $mode > /dev/null
		sleep 5
		./run_ycsb.sh $workload $thread $run $mode
	       	sleep 5
       		done
       	done
done
done
