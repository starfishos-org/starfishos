write cnn_bind_cpu.txt 0-9
write kmeans_bind_cpu.txt 3-11
tiny-cnn -a alexnet.dat -v -o mm-raw.dat -l raw_filelists.txt -t 8 -s 8 &
sleep 5
kmeans.bin -p 100000 -t 8 &
