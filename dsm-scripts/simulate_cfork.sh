#!/bin/bash

program=$1

session_name=$USER-qemu
window_name="window0"
window_start_index=0

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name

## Split the window to 4 panes.
tmux split-window -v -t $session_name:$window_name

## Run the ROS programs sequentially.
tmux send -t $session_name:$window_name.$window_start_index "./dsm-scripts/tests/cfork_prepare.exp $program > exec_log | tee exec_log1" ENTER
sleep 5
tmux send -t $session_name:$window_name.$((window_start_index + 1)) "./dsm-scripts/tests/cfork_restore.exp $program > exec_log | tee exec_log2" ENTER

## Attach the Tmux session to the front.
tmux a -t $session_name
