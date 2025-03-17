#!/bin/bash

# use ./simulayed_cluster build to build and start the program
if [ "$1" == "build" ]; then
	./chbuild build
fi

session_name=$USER-qemu
window_name0="window0"
window_name1="window1"

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name0

## Split the window to 2 panes.
tmux new-window -t $session_name -n $window_name1

tmux send -t $session_name:$window_name0 "./build/simulate.sh 0 | tee exec_log0.ans" ENTER
sleep 1
while ! grep -q "Welcome to ChCore shell!" exec_log0.ans; do
    sleep 0.1
done

tmux send -t $session_name:$window_name1 "./build/simulate.sh 1 | tee exec_log1.ans" ENTER
sleep 1
while ! grep -q "Welcome to ChCore shell!" exec_log1.ans; do
    sleep 0.1
done

tmux send -t $session_name:$window_name1 "write /1/1.txt Hello1 &" ENTER
while ! grep -q "write success" exec_log1.ans; do
    sleep 0.1
done

tmux send -t $session_name:$window_name0 "mmap /1/1.txt &" ENTER

tmux select-window -t $session_name:$window_name0

tmux a -t $session_name
