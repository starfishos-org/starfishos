write linear_regression_bind_cpu.txt 3-11
redis-server redis.conf &
sleep 5
redis-benchmark &
sleep 1
linear_regression.bin -f key_file_100MB.txt -t 8 &
