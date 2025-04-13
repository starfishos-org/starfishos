write leveldb_bind_cpu.txt 0-7
write matrix_bind_cpu.txt 8-15
matrix_multiply.bin -l 3000 -r 3000 -t 8 -c 0 # &
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 &
