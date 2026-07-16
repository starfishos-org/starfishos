write leveldb_bind_cpu.txt 0-11
# Keep the 100,000-entry workload while dividing writes across 8 workers.
leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 --write_num_is_total=1 &
