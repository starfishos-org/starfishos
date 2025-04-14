write leveldb_bind_cpu.txt 0-7
write matrix_multiply_bind_cpu.txt 3-11
matrix_multiply.bin -l 2000 -r 2000 -c 0 -t 8 &
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 &
