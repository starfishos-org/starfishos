write word_count_bind_cpu.txt 8-15
memcached -l 127.0.0.1 -p 123 -t 8 &
sleep 5
word_count.bin -f word_100MB.txt -t 8 # &
memcachetest -h 127.0.0.1:123 -M 1024 -F -t 8 -i 100000 &
