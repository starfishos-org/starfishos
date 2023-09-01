import os, sys
import pandas as pd

# Define the directory path
directory = sys.argv[1]

# Define the prefixes and metrics
prefixes = ['ckpt0.', 'ckpt1.', 'ckpt5.', 'ckpt10.', 'ckpt50.']
metrics = ['P50', 'P95', 'P99']
workloads = ['SET', 'GET']
labels = ['SET', 'GET', 'SETraw', 'GETraw']
# Initialize the dictionary
data = {}

for l in labels:
    data[l] = {}

def parse(words):
    idx = 7
    if words[idx+1] == 'ns':
        p50 = float(words[idx])/1000
    elif words[idx+1] == 'ms':
        p50 = float(words[idx])*1000
    else:
        p50 = float(words[idx])

    idx = 11  
    if words[idx+1] == 'ns':
        p95 = float(words[idx])/1000
    elif words[idx+1] == 'ms':
        p95 = float(words[idx])*1000
    else:
        p95 = float(words[idx])

    idx=13
    if words[idx+1] == 'ns':
        p99 = float(words[idx])/1000
    elif words[idx+1] == 'ms':
        p99 = float(words[idx])*1000
    else:
        p99 = float(words[idx])

    return p50, p95, p99

# Loop through the log files
for prefix in prefixes:
    for l in labels:
        data[l][prefix] = {}

    # Loop through the metrics
    for metric in metrics:
        data['GET'][prefix][metric] = 0
        data['SET'][prefix][metric] = 0

        data['GETraw'][prefix][metric] = []
        data['SETraw'][prefix][metric] = []

    # Find log files with the given prefix
    file_names = [file for file in os.listdir(directory) if file.startswith(prefix)]

    # Loop through the log files
    for file_name in file_names:
        file_path = os.path.join(directory, file_name)
        
        # Read the log file
        with open(file_path, 'r') as file:
            lines = file.readlines()
        
        # Parse the log file and extract the required metrics
        for i in range(0, len(lines)):
            if "Get operations:" in lines[i]:
                words = lines[i+2].split()
                p50, p95, p99 = parse(words)
                # print(p50, p95, p99)
                data['GETraw'][prefix]['P50'].append(p50)
                data['GETraw'][prefix]['P95'].append(p95)
                data['GETraw'][prefix]['P99'].append(p99)
            if "Set operations:" in lines[i]:
                words = lines[i+2].split()
                p50, p95, p99 = parse(words)
                data['SETraw'][prefix]['P50'].append(p50)
                data['SETraw'][prefix]['P95'].append(p95)
                data['SETraw'][prefix]['P99'].append(p99)
# print(data)

for i in workloads:
    for prefix in prefixes:
        for metric in metrics:
            data[i][prefix][metric] = sum(data[i+'raw'][prefix][metric])/len(data[i+'raw'][prefix][metric])

    # Convert the dictionary to a DataFrame
    df = pd.DataFrame.from_dict(data[i])
    # Save the DataFrame as a CSV file
    df.to_csv('./result/memcached-{}.csv'.format(i))
    print(df)
