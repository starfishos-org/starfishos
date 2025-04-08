#!/bin/bash

session_name=$USER-qemu
window_name="window"
num_windows=$1
log_file=$2
program=$3
expected_str=$4
num_numa=4

if [ $# -ne 4 ]; then
    echo "Usage: $0 <num_windows> <log_file> <program> <expected_str>"
    echo "num_windows: the number of kernels to create"
    echo "program: the program to run on the kernels"
    echo "log_file: the log file to save the log"
    echo "expected_str: the string to expect in the log"
    exit 1
fi

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

make clean-dsm
welcome_str="Welcome to ChCore shell!"

echo "num_windows: $num_windows"
echo "program: $program"
## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n 0 "./build/simulate.sh 0 > exec_log | tee exec_log0.log"

sleep 3

kernel_ready 0

for ((i=1; i<$num_windows; i++)); do
    tmux new-window -n $i "./build/simulate.sh $((i % $num_numa)) > exec_log | tee exec_log$i.log"
    sleep 1
done

for ((i=1; i<$num_windows; i++)); do
    kernel_ready $i
done

tmux send -t $session_name:0 "$program" ENTER

while true; do
    grep -q "$expected_str" exec_log0.log
    
    if [ $? -eq 0 ]; then
        grep "$expected_str" exec_log0.log >> $log_file
        break
    fi
    
    sleep 1
done
