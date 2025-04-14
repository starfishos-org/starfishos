write word_count_bind_cpu.txt 0-7
write dbx1000_bind_cpu.txt 3-11
rundb.bin -t8 -r1 -w0 -z0.6 &
sleep 4
word_count.bin -f word_100MB.txt -t 8 &
