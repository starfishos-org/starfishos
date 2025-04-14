write leveldb_bind_cpu.txt 0-11
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8
