#!/bin/bash

# use ./simulayed_cluster build to build and start the program
if [ "$1" == "build" ]; then
	./chbuild build
fi

./dsm-scripts/config_memdev.sh cxl

session_name=$USER-qemu
window_name="window0"

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name

## Split the window to 4 panes.
tmux split-window -v -t $session_name:$window_name

## Run the ROS programs sequentially.
tmux send -t $session_name:$window_name.0 "./build/simulate.sh 0 > exec_log | tee exec_log1" ENTER
sleep 3
tmux send -t $session_name:$window_name.0 "write /0/0.txt Hello0" ENTER
tmux send -t $session_name:$window_name.1 "./build/simulate.sh 1 > exec_log | tee exec_log2" ENTER
sleep 3
tmux send -t $session_name:$window_name.1 "write /1/1.txt Hello1" ENTER

tmux select-pane -t $session_name:$window_name.1

## Attach the Tmux session to the front.
tmux a -t $session_name
