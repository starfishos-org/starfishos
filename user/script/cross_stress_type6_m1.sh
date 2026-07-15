# GeminiGraph reads gemini_bind_cpu.txt.  pagerank_bind_cpu.txt was used by an
# older launcher and leaves the current binary pinned to machine 0.
write gemini_bind_cpu.txt 12-19
pagerank cnr-2000.binedgelist 325557 1 1 &
