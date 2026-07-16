write leveldb_bind_cpu.txt 0-7
# CPU 9 is reserved by the busy-polling service; binding an idle Phoenix
# worker there prevents map_reduce_finalize() from joining the full pool.
write matrix_multiply_bind_cpu.txt 3-8,10-11
matrix_multiply.bin -l 2000 -r 2000 -c 0 -t 8 &
# Keep the 100,000-entry workload while dividing writes across 8 workers.
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 --write_num_is_total=1 &
