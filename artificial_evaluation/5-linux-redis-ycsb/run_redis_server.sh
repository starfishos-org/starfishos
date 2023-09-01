#!/bin/bash
redis_dir=$(pwd)/redis/
mode=$1
if [ "$mode" = "nvm-log" ]; then
	config_file="$(pwd)/redis.aof.conf"
elif [ "$mode" = "disk-log" ]; then
	config_file="$(pwd)/redis.aof.conf"
else
	config_file="$(pwd)/redis.conf"
fi
sudo taskset -c 0 $redis_dir/src/redis-server $config_file --unixsocket /tmp/redis.sock &
