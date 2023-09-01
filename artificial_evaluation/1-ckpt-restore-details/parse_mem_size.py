import sys
import numpy as np
import re
import os
import os.path as op
import pandas as pd

# Define the directory path
directory = sys.argv[1]
outfile = sys.argv[2]

line_begin = "free mem size = "
line_end = " MB"
workloads = ['memcached', 'redis', 'sqlite', 'leveldb', 'kmeans', 'word_count']
modes = ['raw', 'ckpt']
data = {}
data['raw'] = {}
data['cal'] = {}

if __name__ == "__main__":
    file_dir = sys.argv[1]
    outfile = sys.argv[2]
    files = []
    workload = {}
    
    for prefix in workloads:
        data['raw'][prefix] = {}
        data['cal'][prefix] = {}
        for mode in modes:
            # Find log files with the given prefix
            file_names = [file for file in os.listdir(directory) if file.startswith(prefix + '.' + mode)]

            data['raw'][prefix][mode] = []

            # Loop through the log files
            for file_name in file_names:
                file_path = os.path.join(directory, file_name)

                with open(file_path, 'rt') as f:
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
                diff = max_mem_size - min_mem_size
                if diff > 0:
                    data['raw'][prefix][mode].append(diff)
            # print(prefix, mode, data['raw'][prefix][mode])
            l = len(data['raw'][prefix][mode])
            if l > 0:
                data['cal'][prefix][mode] = sum(data['raw'][prefix][mode])/l
            else:
                data['cal'][prefix][mode] = 0
    
    for prefix in workloads:
        data['cal'][prefix]['ckpt'] = data['cal'][prefix]['ckpt'] - data['cal'][prefix]['raw']
    
    # Convert the dictionary to a DataFrame
    df = pd.DataFrame.from_dict(data['cal'])
    # Save the DataFrame as a CSV file
    df.to_csv(outfile)
    print(df)            
