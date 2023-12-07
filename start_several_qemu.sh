#!/bin/bash

machine_num=$1

for i in $(seq 1 $machine_num); do
	rm /dev/shm/shmem$i
	./build/simulation.sh $i $machine_num
done
