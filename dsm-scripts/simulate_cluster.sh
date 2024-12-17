#!/bin/bash

# use ./simulayed_cluster build to build and start the program
if [ "$1" == "build" ]; then
	./chbuild build
fi

./dsm-scripts/config_memdev.sh cxl
./dsm-scripts/config.sh

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s mywork -n window0

## Split the window to 4 panes.
tmux split-window -h -t mywork:window0
# tmux split-window -v -t mywork:window0.0
# tmux split-window -v -t mywork:window0.1

## Run the ROS programs sequentially.
tmux send -t mywork:window0.0 "./build/simulate.sh > exec_log | tee exec_log1" ENTER
sleep 3
tmux send -t mywork:window0.1 "./build/simulate.sh > exec_log | tee exec_log2" ENTER
# tmux send -t mywork:window0.2 "source setup.bash && roslaunch prog3 p3.launch" ENTER
# tmux send -t mywork:window0.3 "source setup.bash && roslaunch prog4 p4.launch" ENTER

## Attach the Tmux session to the front.
tmux a -t mywork
