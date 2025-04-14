write dbx1000_bind_cpu.txt 0-7
write matrix_multiply_bind_cpu.txt 8-15
matrix_multiply.bin -l 2000 -r 2000 -t 8 -c 0 # &
rundb.bin -t8 -r1 -w0 -z0.6 &
