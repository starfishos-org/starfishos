#!/bin/bash

session_name=$USER-qemu
window_name="window"
num_windows=$1
program=$2

if [ $# -ne 2 ]; then
    echo "Usage: $0 <num_windows> <program>"
    echo "num_windows: the number of kernels to create"
    echo "program: the program to run on the kernels"
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

for ((i=1; i<$num_windows; i++)); do
    tmux new-window -n $i "./build/simulate.sh $i > exec_log | tee exec_log$i.log"
    sleep 1
done

for ((i=0; i<$num_windows; i++)); do
    kernel_ready $i
done

tmux send -t $session_name:0 "$program" ENTER

tmux a -t $session_name:0
