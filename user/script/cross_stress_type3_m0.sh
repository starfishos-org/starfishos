write word_count_bind_cpu.txt 8-15
redis-server redis.conf &
sleep 5
redis-benchmark &
word_count.bin -f word_100MB.txt -t 8 # &
