#!/bin/bash

session_name=$USER-qemu
window_name0="window0"
window_name1="window1"

## Create a Tmux session "mywork" in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name0

## Split the window to 2 panes.
tmux new-window -t $session_name -n $window_name1

tmux send -t $session_name:$window_name0 "./build/simulate.sh 0 | tee exec_log0.ans" ENTER
sleep 3

tmux send -t $session_name:$window_name1 "./build/simulate.sh 1 | tee exec_log1.ans" ENTER

tmux send -t $session_name:$window_name0 "leveldb-dbbench --benchmarks=fillbatch --num=10000 --db=/tmp" ENTER
tmux send -t $session_name:$window_name0 "leveldb-dbbench --benchmarks=fillbatch --num=10000 --db=/1/tmp &" ENTER

tmux select-window -t $session_name:$window_name0

## Attach the Tmux session to the front.
tmux a -t $session_name
