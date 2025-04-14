#!/bin/bash

session_name=$USER-qemu
window_name="window"
script_path=$1
expected_str="done"

if [ $# -ne 1 ]; then
    echo "Usage: $0 <script_path>"
    echo "script_path: the path to the script to run"
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

make clean-dsm-meta
welcome_str="Welcome to ChCore shell!"

echo "num_windows: $num_windows"
echo "program: $program"
## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n 0 "./build/simulate.sh 0"

sleep 3

kernel_ready 0

tmux send -t $session_name:0 "source $script_path" ENTER

while true; do
    grep -q "$expected_str" exec_log0.log
    
    if [ $? -eq 0 ]; then
        grep "$expected_str" exec_log0.log >> $log_file
        break
    fi
    
    sleep 1
done
