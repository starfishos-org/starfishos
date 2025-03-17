#!/bin/bash

# use ./simulayed_cluster build to build and start the program
if [ "$1" == "build" ]; then
	./chbuild build
fi

session_name=$USER-qemu
window_name="window0"

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name

## Split the window to 4 panes.
tmux split-window -v -t $session_name:$window_name

## Run the ROS programs sequentially.
tmux send -t $session_name:$window_name.0 "./build/simulate.sh 0 | tee exec_log1.ans" ENTER

sleep 2

tmux send -t $session_name:$window_name.1 "./build/simulate.sh 1 | tee exec_log2.ans" ENTER

sleep 2

tmux send -t $session_name:$window_name.1 "write /1/1 Hello1\r" ENTER

# tmux send -t $session_name:$window_name.0 "leveldb-dbbench --benchmarks=fillbatch --num=1000000 --db=/1/tmp &" ENTER

tmux select-pane -t $session_name:$window_name.0

## Attach the Tmux session to the front.
tmux a -t $session_name
