import sys
import numpy as np
import re
import os
import os.path as op


line_begin = "free mem size = "
line_end = " MB"

if __name__ == "__main__":
    file_dir = sys.argv[1]
    outfile = sys.argv[2]
    files = []
    workload = {}
    for file in os.listdir(file_dir):
        workload[file_dir+"/"+file] = file.split('.')[0]
        files.append(file_dir+"/"+file)
    files.sort()

    out = open(outfile, "w+") 
    for file in files:
        with open(file, 'rt') as f:
            lines=f.readlines()
        min_mem_size = sys.maxsize
        max_mem_size = 0
        for line in lines:
            index_begin = line.find(line_begin)
            index_end = line.find(line_end)
            if index_begin >= 0:
                try:
                    mem_size = int(line[index_begin + len(line_begin): index_end])
                except ValueError:
                    continue
                # print(mem_size)
                if mem_size < min_mem_size:
                    min_mem_size = mem_size
                if mem_size > max_mem_size:
                    max_mem_size = mem_size

        out.write(("%s:\tmin_mem_size=%d,\tmax_mem_size=%d,\tdiff=%d\n") % (workload[file], min_mem_size, max_mem_size, max_mem_size - min_mem_size))
    out.close()
                
