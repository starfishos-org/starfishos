#!/bin/bash

set -x

# use ./simulayed_cluster build to build and start the program
if [ "$1" == "build" ]; then
	./chbuild build
fi

session_name=$USER-qemu
window_name="window0"
window_start_index=0

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name

## Split the window to 4 panes.
tmux split-window -h -t $session_name:$window_name
tmux split-window -v -t $session_name:$window_name
tmux split-window -v -t $session_name:$window_name.$window_start_index

## Run the ROS programs sequentially.
tmux send -t $session_name:$window_name.$window_start_index "./build/simulate.sh 0 > exec_log | tee exec_log1" ENTER
sleep 1
tmux send -t $session_name:$window_name.$((window_start_index + 1)) "./build/simulate.sh 1 > exec_log | tee exec_log2" ENTER
tmux send -t $session_name:$window_name.$((window_start_index + 2)) "./build/simulate.sh 2 > exec_log | tee exec_log3" ENTER
tmux send -t $session_name:$window_name.$((window_start_index + 3)) "./build/simulate.sh 3 > exec_log | tee exec_log4" ENTER

tmux select-pane -t $session_name:$window_name.0

## Attach the Tmux session to the front.
tmux a -t $session_name
