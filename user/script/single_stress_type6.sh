write pagerank_bind_cpu.txt 0-9
write string_match_bind_cpu.txt 3-11
string_match.bin -f word_100MB.txt -t 8 &
pagerank cnr-2000.binedgelist 325557 1 8 8 &
