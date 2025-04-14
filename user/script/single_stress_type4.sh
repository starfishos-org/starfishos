write pca_bind_cpu.txt 3-11
memcached -l 127.0.0.1 -p 123 -t 8 &
sleep 5
pca.bin -c 1000 -r 1000 -t 8 &
memcachetest -h 127.0.0.1:123 -M 1024 -F -t 8 -i 100000 &
