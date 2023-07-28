import os, sys
import pandas as pd

# Define the directory path
ROOT_DIR = sys.argv[1]

# Define the prefixes and metrics
prefixes = ['raw.', 'plusckpt.', 'pluspf.','plusmemcpy.', 'ckpt1ms.']
# workloads = ['memcached', 'redis', 'kmeans', 'pca']
workloads = []
items = os.listdir(ROOT_DIR)
for item in items:
    item_path = os.path.join(ROOT_DIR, item)
    if os.path.isdir(item_path):
        workloads.append(item)

# Initialize the dictionary
data = {}
data['raw'] = {}
data['cal'] = {}

def parse_exe_time(workload, lines):
    for l in lines:
        if workload == 'memcached':
            if 'Tot: ' in l:
                return float(l.split()[1])
        if workload == 'redis':
            if 'completed in' in l:
                return float(l.split()[4])
        if workload == 'pca' or workload == 'kmeans':
            if 'library: ' in l and 'inter library: ' not in l:
                return float(l.split()[1])
    return 0


# Loop through the log files
for workload in workloads:
    data['raw'][workload] = {}
    data['cal'][workload] = {}

    # Loop through the metrics
    for prefix in prefixes:
        data['raw'][workload][prefix] = []
        data['cal'][workload][prefix] = 0

for workload in workloads:
    for prefix in prefixes:
        # Find log files with the given prefix
        directory = ROOT_DIR + '/' + workload
        file_names = [file for file in os.listdir(directory) if file.startswith(prefix)]

        # Loop through the log files
        for file_name in file_names:
            file_path = os.path.join(directory, file_name)
            # Read the log file
            with open(file_path, 'r') as file:
                lines = file.readlines()
            
            # Parse the log file and extract the required metrics
            time = parse_exe_time(workload, lines)
            data['raw'][workload][prefix].append(time)

for prefix in prefixes:
    for workload in workloads:
        length = len(data['raw'][workload][prefix])
        if length != 0:
            data['cal'][workload][prefix] = sum(data['raw'][workload][prefix])/length

# Convert the dictionary to a DataFrame
df = pd.DataFrame.from_dict(data['cal']).transpose()
# 'raw.', 'plusckpt.', 'pluspf.','plusmemcpy.', 'ckpt1ms.'
df = df.rename(columns={'raw.': 'base (no checkpoint)',
         'plusckpt.': '+ checkpoint', 
         'pluspf.': '+ page fault', 
         'plusmemcpy.': '+ page memcpy', 
         'ckpt1ms.': '+ hybrid copy'})
print(df)
# Save the DataFrame as a CSV file
df.to_csv('./result/hybrid-mem.csv')
