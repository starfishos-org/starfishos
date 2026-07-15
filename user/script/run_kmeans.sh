# A single-machine guest exposes local CPUs 0-11.  Phoenix currently consumes
# only the first -t entries, but keep the bind file inside the actual topology
# so a future thread-count change cannot target CPUs owned by absent machines.
write kmeans_bind_cpu.txt 0-11
kmeans.bin -p 100000 -t 8
