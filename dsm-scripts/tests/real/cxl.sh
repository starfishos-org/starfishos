#!/bin/bash

machine=$1

session_name=$USER-qemu
window_name="window"

# library
matrix="matrix_multiply.bin -l 3000 -r 3000 -t 8 -c 0 &"
# library
linear_regression="linear_regression.bin -f key_file_100MB.txt -t 8 &"
# library
pca="pca.bin -c 2000 -r 2000 -t 8 &"
# library
word_count="word_count.bin -f word_100MB.txt -t 8 &"
# MB/s
leveldb="leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp --threads=8 &"
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

tmux new-window -n 1 "./build/simulate.sh 1"

kernel_ready 1

if [ $machine == "1" ]; then
    echo "RUNNING MATRIX & LEVELDB & LINEAR REGRESSION"

    tmux send -t $session_name:0 "write leveldb_bind_cpu.txt 0-7" ENTER
    sleep 1
    tmux send -t $session_name:0 "write matrix_bind_cpu.txt 8-15" ENTER
    sleep 1
    tmux send -t $session_name:1 "write linear_regression_bind_cpu.txt 16-23" ENTER
    sleep 1
    tmux send -t $session_name:0 "$matrix" ENTER
    tmux send -t $session_name:0 "$leveldb" ENTER
    tmux send -t $session_name:1 "$linear_regression" ENTER
fi

if [ $machine == "2" ]; then
    echo "RUNNING LINEAR REGRESSION & DBX1000 & PCA"

    tmux send -t $session_name:0 "write dbx1000_bind_cpu.txt 0-7" ENTER
    sleep 1
    tmux send -t $session_name:0 "write linear_regression_bind_cpu.txt 8-15" ENTER
    sleep 1
    tmux send -t $session_name:1 "write pca_bind_cpu.txt 16-23" ENTER
    sleep 1
    tmux send -t $session_name:0 "$linear_regression" ENTER
    tmux send -t $session_name:0 "$dbx1000" ENTER
    tmux send -t $session_name:1 "$pca" ENTER
fi

if [ $machine == "3" ]; then
    echo "RUNNING PCA & REDIS & WORD COUNT"

    tmux send -t $session_name:0 "write redis_bind_cpu.txt 0-7" ENTER
    sleep 1
    tmux send -t $session_name:0 "write pca_bind_cpu.txt 8-15" ENTER
    sleep 1
    tmux send -t $session_name:0 "write word_count_bind_cpu.txt 16-23" ENTER
    sleep 1
    tmux send -t $session_name:0 "$redis_server" ENTER
    sleep 5
    tmux send -t $session_name:0 "$pca" ENTER
    tmux send -t $session_name:0 "$redis_benchmark" ENTER
    tmux send -t $session_name:1 "$word_count" ENTER
fi

if [ $machine == "4" ]; then
    echo "RUNNING WORD COUNT & MEMCACHED"

    tmux send -t $session_name:0 "write word_count_bind_cpu.txt 0-7" ENTER
    sleep 1
    tmux send -t $session_name:0 "write memcached_bind_cpu.txt 3-11" ENTER
    sleep 1
    tmux send -t $session_name:0 "$memcached_server" ENTER
    sleep 5
    tmux send -t $session_name:0 "$word_count" ENTER
    tmux send -t $session_name:0 "$memcached_test" ENTER
fi

tmux a -t $session_name:0
