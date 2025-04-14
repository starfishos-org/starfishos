# get avg
memcached -l 127.0.0.1 -p 123 -t 8 &
sleep 5
memcachetest -h 127.0.0.1:123 -M 1024 -F -t 8 -i 100000
