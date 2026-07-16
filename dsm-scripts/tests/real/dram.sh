#!/bin/bash

machine=$1

session_name=$USER-qemu
window_name="window"

# library time
matrix="matrix_multiply.bin -l 3000 -r 3000 -t 8 -c 0 &"
# library time
linear_regression="linear_regression.bin -f key_file_100MB.txt -t 8 &"
# library time
pca="pca.bin -c 2000 -r 2000 -t 8 &"
# library time
word_count="word_count.bin -f word_100MB.txt -t 8 &"
# MB/s
leveldb="leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 --write_num_is_total=1 &"
# thp
dbx1000="rundb.bin -t8 -r1 -w0 -z0.6 &"
# op/s
redis_server="redis-server redis.conf &"
redis_benchmark="redis-benchmark &"
# get avg
memcached_server="memcached -l 127.0.0.1 -p 123 -t 8 &"
memcached_test="memcachetest -h 127.0.0.1:123 -M 1024 -F -t 8 -i 100000 &"

kernel_ready() {
  echo "Kernel $1 is creating..."
  tmux select-window -t $1

  while true; do
      tmux capture-pane -pS -1000 | grep -q "$welcome_str"
      
      if [ $? -eq 0 ]; then
          break
      fi
      
      sleep 1
  done
  echo "Kernel $1 is ready"
}

make clean-dsm-meta
welcome_str="Welcome to ChCore shell!"

echo "machine type: $machine"
## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n 0 "./build/simulate.sh 0"

sleep 3

kernel_ready 0

tmux send -t $session_name:0 "source single_stress_type$machine.sh" ENTER

tmux a -t $session_name
