write leveldb_bind_cpu.txt 0-7
write pca_bind_cpu.txt 8-15
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 &
pca.bin -c 1000 -r 1000 -t 8 # &
