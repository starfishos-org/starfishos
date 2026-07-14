#!/bin/bash

set -x

# use ./simulate_8clusters.sh build to build and start the program
if [ "$1" == "build" ]; then
	./scripts/chbuild-with-fallback.sh build
fi

make start-ivshmem-server
make clean-dsm

session_name=$USER-qemu
window_name="window0"
window_start_index=0

## Create a Tmux session in a window "window0" started in the background.
tmux new -d -s $session_name -n $window_name

## Split the window to 8 panes (4 columns x 2 rows).
tmux split-window -h -t $session_name:$window_name
tmux split-window -h -t $session_name:$window_name.$window_start_index
tmux split-window -h -t $session_name:$window_name.$((window_start_index + 1))
tmux split-window -v -t $session_name:$window_name.$window_start_index
tmux split-window -v -t $session_name:$window_name.$((window_start_index + 2))
tmux split-window -v -t $session_name:$window_name.$((window_start_index + 1))
tmux split-window -v -t $session_name:$window_name.$((window_start_index + 3))

## Run simulate.sh on each pane sequentially.
for ((i = 0; i < 8; i++)); do
	tmux send -t $session_name:$window_name.$((window_start_index + i)) "./build/simulate.sh $i" ENTER
	[ $i -lt 7 ] && sleep 5
done

tmux select-pane -t $session_name:$window_name.0

## Attach the Tmux session to the front.
tmux a -t $session_name
